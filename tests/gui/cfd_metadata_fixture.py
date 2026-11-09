# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare mixed valid/invalid saved runs and a real-GUI startup check; no solver needed.

python tests/gui/cfd_metadata_fixture.py --cmd build/release/bin/aerocad-cmd --output /tmp/cad91
AEROCAD_CFD_DIR=/tmp/cad91/runs xvfb-run build/release/bin/AeroCAD --ui-script /tmp/cad91/metadata.ui
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
    commands = [{"name": "document.import", "params": {"path": str(model)}},
                {"name": "simulation.run", "params": {"id": 1, "prepareOnly": True,
                                                     "label": "Valid prepared run"}}]
    result = subprocess.run([str(args.cmd.resolve()), "-"], input=json.dumps(commands), text=True,
                            capture_output=True, check=True, env=dict(os.environ, AEROCAD_CFD_DIR=str(runs)))
    (output / "prepare.jsonl").write_text(result.stdout)
    responses = [json.loads(line) for line in result.stdout.splitlines()]
    if len(responses) != 2 or any(not row["ok"] for row in responses):
        raise RuntimeError("Could not prepare fixture: " + result.stdout)
    valid = responses[-1]["result"]["id"]
    for name, content in (("broken-backend", '{"backend":42}'), ("broken-source", '{"source":[]}')):
        directory = runs / name
        directory.mkdir(exist_ok=True)
        (directory / "run.json").write_text(content)
    lines = ["# Synthetic saved metadata only; no simulation engine is launched.",
             'ui.log("GUI_READY")', "ui.wait(400)", 'ui.clickText("SIMULATE")', "ui.wait(200)",
             'ui.clickText("Wind Tunnel")', "ui.wait(500)", "simulationPanel.refreshRuns()",
             '(() => { const runs = simulationPanel.runs; ui.log(JSON.stringify(runs)); '
             'if (runs.length !== 3) ui.fail("Expected one valid and two invalid runs"); '
             'for (const id of ["broken-backend", "broken-source"]) { '
             'const r = runs.find(x => x.id === id); '
             'if (!r || r.state !== "failed" || !r.message.includes("run.json") '
             '|| !r.message.includes("restore")) ui.fail("Missing per-run recovery diagnostic: " + id); } })()',
             'simulationPanel.selectRun("broken-backend")', 'ui.clickText("Results")', "ui.wait(200)",
             'if (simulationPanel.results !== null) ui.fail("Invalid run exposed results")',
             '(() => { const find = item => item.objectName === "simulationResultsUnavailable" ? item '
             ': Array.from(item.children || []).map(find).find(x => x); const error = find(simulationPanel); '
             'if (!error || !error.visible || !error.text.includes("run.json")) '
             'ui.fail("Recovery instructions are not visible in Results"); })()',
             f"ui.shot({json.dumps(str(output / 'invalid-run.png'))})",
             f"simulationPanel.selectRun({json.dumps(valid)})", "ui.wait(200)",
             '(() => { const r = simulationPanel.results; '
             'if (!r || !r.conditions || r.run.state !== "interrupted") '
             'ui.fail("Valid prepared run is unavailable"); ui.log("VALID_RUN_AVAILABLE"); })()',
             f"ui.shot({json.dumps(str(output / 'valid-run.png'))})"]
    (output / "metadata.ui").write_text("\n".join(lines) + "\n")
    print(f"Set AEROCAD_CFD_DIR={runs} and run AeroCAD --ui-script {output / 'metadata.ui'}")


if __name__ == "__main__":
    main()
