#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate AeroCAD exports with independent readers.

Builds a few models through `aerocad-cmd` (the same commands the UI and the MCP server
use), exports them as STL, 3MF and STEP, and reads the files back with tools that do not
share code with AeroCAD's writers:

- lib3mf (3MF Consortium reference library): 3MF package and model validity, unit,
  object names, manifold and oriented meshes.
- trimesh: STL and 3MF parsing, watertightness, winding, volume, bounds.
- gmsh: STEP import (gmsh's own build of Open CASCADE), solid count, volume, bounds.
- A plain-text check that the STEP file declares millimetres.

Missing Python packages are reported and skipped, so the script runs with whatever is
installed:  python3 -m pip install lib3mf trimesh numpy networkx lxml gmsh

Usage:
  tools/validate_exports.py --cmd build/release/bin/aerocad-cmd [--keep DIR]

Exit code 0 when every available check passes, 1 otherwise. Not part of CI (it needs
packages CI does not install); run it locally after touching the exporters.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FAILURES: list[str] = []


def check(condition: bool, message: str) -> None:
    print(("  ok    " if condition else "  FAIL  ") + message)
    if not condition:
        FAILURES.append(message)


def close(a: float, b: float, rel: float = 1e-3, abs_tol: float = 1e-3) -> bool:
    return math.isclose(a, b, rel_tol=rel, abs_tol=abs_tol)


def run_script(cmd: Path, steps: list[dict]) -> list[dict]:
    proc = subprocess.run([str(cmd), "-"], input=json.dumps(steps), capture_output=True, text=True)
    results = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
    if proc.returncode != 0:
        raise SystemExit(f"aerocad-cmd failed ({proc.returncode}):\n{proc.stderr}")
    return results


# --- Models -----------------------------------------------------------------------------

# Each model: commands that build it, expected solid volume (mm^3, None = take the exact
# B-rep volume the feature commands report), bounds size and part count, and which
# formats it can be exported to.
def models() -> list[dict]:
    tetra = REPO / "tests" / "data" / "tetrahedron.stl"
    return [
        {
            "name": "tetrahedron_mesh",
            "steps": [{"name": "document.import", "params": {"path": str(tetra)}}],
            "volume": 1000.0 / 6.0,  # right tetrahedron with 10 mm legs
            "size": None,
            "parts": 1,
            "formats": ["stl", "3mf"],
        },
        {
            "name": "box_and_cylinder",
            "steps": [
                {"name": "feature.box", "params": {"size": [30, 20, 10]}},
                {"name": "feature.cylinder",
                 "params": {"center": [60, 10, 0], "radius": 5, "height": 40, "operation": "new_body"}},
            ],
            "volume": 30 * 20 * 10 + math.pi * 25 * 40,
            "size": [65, 20, 40],
            "parts": 2,
            "formats": ["stl", "3mf", "step"],
        },
        {
            "name": "filleted_box",
            "steps": [
                {"name": "feature.box", "params": {"size": [40, 40, 20]}},
                {"name": "feature.fillet", "params": {"body": 1, "edges": list(range(12)), "radius": 3}},
            ],
            "volume": None,  # exact B-rep volume, reported by the fillet feature below
            "size": [40, 40, 20],
            "parts": 1,
            "formats": ["stl", "3mf", "step"],
        },
    ]


# --- Validators -------------------------------------------------------------------------

def validate_3mf_lib3mf(path: Path, model: dict) -> None:
    try:
        import lib3mf  # type: ignore
    except ImportError:
        print("  skip  lib3mf not installed")
        return
    wrapper = lib3mf.get_wrapper()
    m = wrapper.CreateModel()
    reader = m.QueryReader("3mf")
    reader.SetStrictModeActive(True)
    reader.ReadFromFile(str(path))
    warnings = [reader.GetWarning(i) for i in range(reader.GetWarningCount())]
    check(not warnings, f"lib3mf strict read without warnings {warnings if warnings else ''}")
    check(m.GetUnit() == lib3mf.ModelUnit.MilliMeter, "lib3mf: unit is millimeter")
    objects = m.GetMeshObjects()
    count = 0
    while objects.MoveNext():
        obj = objects.GetCurrentMeshObject()
        count += 1
        check(obj.GetTriangleCount() > 0, f"lib3mf: object '{obj.GetName()}' has triangles")
        check(obj.IsManifoldAndOriented(), f"lib3mf: object '{obj.GetName()}' is manifold and oriented")
        check(bool(obj.GetName()), "lib3mf: object has a name")
    check(count == model["parts"], f"lib3mf: {count} mesh objects (expected {model['parts']})")
    items = m.GetBuildItems()
    n_items = 0
    while items.MoveNext():
        n_items += 1
    check(n_items == model["parts"], f"lib3mf: {n_items} build items (expected {model['parts']})")


def validate_mesh_trimesh(path: Path, model: dict) -> None:
    try:
        import trimesh  # type: ignore
    except ImportError:
        print("  skip  trimesh not installed")
        return
    loaded = trimesh.load(str(path), force="scene")
    meshes = list(loaded.geometry.values()) if hasattr(loaded, "geometry") else [loaded]
    mesh = trimesh.util.concatenate(meshes) if len(meshes) > 1 else meshes[0]
    if path.suffix == ".stl":
        mesh.merge_vertices()
    check(mesh.is_watertight, "trimesh: watertight")
    check(mesh.is_winding_consistent, "trimesh: consistent winding")
    check(mesh.volume > 0, f"trimesh: positive volume ({mesh.volume:.3f} mm^3)")
    # Same mesh as AeroCAD measured before writing (float32 coordinates in STL).
    check(close(mesh.volume, model["mesh_volume"], rel=1e-5),
          f"trimesh: volume {mesh.volume:.3f} == AeroCAD's mesh volume {model['mesh_volume']:.3f} mm^3")
    # Tessellated curved surfaces are slightly smaller than the exact solid.
    check(close(mesh.volume, model["volume"], rel=2e-2),
          f"trimesh: volume {mesh.volume:.2f} ~ exact {model['volume']:.2f} mm^3")
    if model["size"] is not None:
        size = mesh.bounds[1] - mesh.bounds[0]
        check(all(close(a, b, rel=1e-3, abs_tol=1e-2) for a, b in zip(size, model["size"])),
              f"trimesh: size {[round(float(s), 3) for s in size]} ~ {model['size']} mm")


def validate_step_text(path: Path) -> None:
    text = path.read_text(errors="replace")
    check(text.startswith("ISO-10303-21;"), "STEP: ISO-10303-21 header")
    check(re.search(r"SI_UNIT\s*\(\s*\.MILLI\.\s*,\s*\.METRE\.\s*\)", text) is not None,
          "STEP: length unit is millimetre")
    check("AUTOMOTIVE_DESIGN" in text, "STEP: AP214 (AUTOMOTIVE_DESIGN) schema")


def validate_step_gmsh(path: Path, model: dict) -> None:
    try:
        import gmsh  # type: ignore
    except ImportError:
        print("  skip  gmsh not installed")
        return
    gmsh.initialize(["-noenv"], readConfigFiles=False)
    try:
        gmsh.option.setNumber("General.Terminal", 0)
        # gmsh converts STEP to its default unit (mm) unless told otherwise.
        gmsh.option.setString("Geometry.OCCTargetUnit", "MM")
        gmsh.model.occ.importShapes(str(path))
        gmsh.model.occ.synchronize()
        solids = gmsh.model.getEntities(3)
        check(len(solids) == model["parts"], f"gmsh: {len(solids)} solids (expected {model['parts']})")
        volume = sum(gmsh.model.occ.getMass(3, tag) for _, tag in solids)
        check(close(volume, model["volume"], rel=1e-6), f"gmsh: volume {volume:.3f} == {model['volume']:.3f} mm^3")
        if model["size"] is not None:
            xmin, ymin, zmin, xmax, ymax, zmax = gmsh.model.getBoundingBox(-1, -1)
            size = [xmax - xmin, ymax - ymin, zmax - zmin]
            check(all(close(a, b, rel=1e-3, abs_tol=0.1) for a, b in zip(size, model["size"])),
                  f"gmsh: size {[round(s, 3) for s in size]} ~ {model['size']} mm")
    finally:
        gmsh.finalize()


# --- Main -------------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cmd", type=Path, default=REPO / "build" / "release" / "bin" / "aerocad-cmd",
                        help="path to the aerocad-cmd executable")
    parser.add_argument("--keep", type=Path, help="write the exported files to this folder and keep them")
    args = parser.parse_args()
    if not args.cmd.exists():
        print(f"aerocad-cmd not found at {args.cmd}; build target aerocad_cmd first", file=sys.stderr)
        return 2

    # document.export only accepts absolute paths.
    out = args.keep.resolve() if args.keep else Path(tempfile.mkdtemp(prefix="aerocad_validate_"))
    out.mkdir(parents=True, exist_ok=True)
    try:
        for model in models():
            print(f"\n== {model['name']}")
            steps = list(model["steps"])
            steps.append({"name": "document.parts"})
            for fmt in model["formats"]:
                steps.append({"name": "document.export",
                              "params": {"path": str(out / f"{model['name']}.{fmt}"), "requireValid": True}})
            results = run_script(args.cmd, steps)
            parts = next(r for r in results if r["command"] == "document.parts")["result"]["parts"]
            check(len(parts) == model["parts"], f"{len(parts)} parts built (expected {model['parts']})")
            if model["size"] is None:
                box = [p["bounds"]["size"] for p in parts][0]
                model["size"] = box
            # Exact volumes: the latest volume each feature command reported per body.
            brep = {}
            for r in results:
                for body in r["result"].get("bodies", []) if r["command"].startswith("feature.") else []:
                    brep[body["id"]] = body["volume"]
            if model["volume"] is None:
                check(bool(brep), "feature commands reported exact body volumes")
                model["volume"] = sum(brep.values())
            elif brep:
                check(close(sum(brep.values()), model["volume"], rel=1e-9),
                      f"AeroCAD B-rep volume {sum(brep.values()):.3f} == expected {model['volume']:.3f} mm^3")
            for r in results:
                if r["command"] == "document.export":
                    check(not r["result"]["warnings"], f"{r['result']['format']}: no mesh warnings from AeroCAD")
                    if r["result"]["checks"]:
                        model["mesh_volume"] = sum(c["volume"] for c in r["result"]["checks"])

            for fmt in model["formats"]:
                path = out / f"{model['name']}.{fmt}"
                print(f"-- {path.name}")
                if fmt == "3mf":
                    validate_3mf_lib3mf(path, model)
                    validate_mesh_trimesh(path, model)
                elif fmt == "stl":
                    validate_mesh_trimesh(path, model)
                elif fmt == "step":
                    validate_step_text(path)
                    validate_step_gmsh(path, model)
    finally:
        if not args.keep:
            shutil.rmtree(out, ignore_errors=True)

    print()
    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed:")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("All checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
