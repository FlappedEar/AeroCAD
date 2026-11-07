#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerate the test model set in tests/data/models.

Each model is built through `aerocad-cmd` (the same commands the UI and the MCP server
use) and written as STEP, STL and 3MF by AeroCAD's exporters. The OBJ copy is written by
this script from the STL triangles (shared vertices, quads not used), so the OBJ reader is
tested on a file that AeroCAD did not write itself.

The files are committed; run this only when a model changes:
  tools/make_test_models.py --cmd build/dev/bin/aerocad-cmd

See tests/data/models/README.md for what each model is for.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "tests" / "data" / "models"

# Dome: a revolved profile, 240 mm across and about 46 mm tall. A smooth doubly curved
# top (like a body panel or mirror housing), a 15 mm vertical rim and a flat bottom.
MODELS = {
    "dome": [
        {"name": "sketch.create", "params": {"plane": "XZ", "curves": [
            {"type": "line", "start": [0, 0], "end": [120, 0]},
            {"type": "line", "start": [120, 0], "end": [120, 15]},
            {"type": "arc", "start": [120, 15], "through": [70, 38], "end": [0, 45]},
            {"type": "line", "start": [0, 45], "end": [0, 0]}]}},
        {"name": "feature.revolve", "params": {"sketch": 1, "axis": "Z"}},
    ],
}


def dome_height(r: float) -> float:
    """Top of the dome at radius r (mm): the profile arc through (120, 15), (70, 38), (0, 45)."""
    cx, cz, radius = 16.416666666667, -144.3333333333, 190.0437266935
    return cz + math.sqrt(radius * radius - (r - cx) ** 2)


def write_scan(path: Path) -> None:
    """dome_scan.stl: the top of the dome as a scanner sees it. Open (no rim or bottom),
    a 3 mm grid with a ragged outline, 0.05 mm Gaussian noise and a hole where a
    reflection was lost. Seeded, so the file is reproducible."""
    rng = random.Random(22)
    step, reach = 3.0, 118.0
    n = int(reach // step)
    points: dict[tuple[int, int], tuple[float, float, float]] = {}
    for i in range(-n, n + 1):
        for j in range(-n, n + 1):
            x, y = i * step, j * step
            r = math.hypot(x, y)
            if r <= reach:
                points[(i, j)] = (x, y, dome_height(r) + rng.gauss(0.0, 0.05))
    triangles = []
    for (i, j) in points:
        quad = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
        if not all(q in points for q in quad):
            continue
        a, b, c, d = (points[q] for q in quad)
        for tri in ((a, b, c), (a, c, d)):
            cx = sum(p[0] for p in tri) / 3
            cy = sum(p[1] for p in tri) / 3
            if math.hypot(cx - 70.0, cy - 20.0) < 8.0:
                continue  # the hole
            triangles.append(tri)
    data = bytearray(b"dome_scan: tools/make_test_models.py, units mm".ljust(80, b" "))
    data += struct.pack("<I", len(triangles))
    for tri in triangles:
        data += struct.pack("<3f", 0.0, 0.0, 0.0)
        for v in tri:
            data += struct.pack("<3f", *v)
        data += b"\0\0"
    path.write_bytes(bytes(data))


def run_script(cmd: Path, steps: list[dict]) -> None:
    proc = subprocess.run([str(cmd), "-"], input=json.dumps(steps), capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"aerocad-cmd failed ({proc.returncode}):\n{proc.stderr}")


def read_binary_stl(path: Path) -> list[tuple[tuple[float, float, float], ...]]:
    data = path.read_bytes()
    count = struct.unpack_from("<I", data, 80)[0]
    triangles = []
    for i in range(count):
        values = struct.unpack_from("<12f", data, 84 + i * 50)
        triangles.append((values[3:6], values[6:9], values[9:12]))
    return triangles


def write_obj(triangles, path: Path, name: str) -> None:
    index: dict[tuple[float, float, float], int] = {}
    vertices: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    for tri in triangles:
        face = []
        for v in tri:
            if v not in index:
                index[v] = len(vertices) + 1
                vertices.append(v)
            face.append(index[v])
        faces.append(tuple(face))
    lines = [f"# {name}: written by tools/make_test_models.py, units mm", f"o {name}"]
    lines += [f"v {x:.6g} {y:.6g} {z:.6g}" for x, y, z in vertices]
    lines += [f"f {a} {b} {c}" for a, b, c in faces]
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cmd", type=Path, default=REPO / "build" / "dev" / "bin" / "aerocad-cmd",
                        help="path to the aerocad-cmd executable")
    args = parser.parse_args()
    if not args.cmd.exists():
        print(f"aerocad-cmd not found at {args.cmd}; build target aerocad_cmd first", file=sys.stderr)
        return 1

    OUT.mkdir(parents=True, exist_ok=True)
    for name, steps in MODELS.items():
        exports = [{"name": "document.export", "params": {"path": str(OUT / f"{name}.{ext}")}}
                   for ext in ("step", "stl", "3mf")]
        run_script(args.cmd, steps + exports)
        write_obj(read_binary_stl(OUT / f"{name}.stl"), OUT / f"{name}.obj", name)
        print(f"{name}: " + ", ".join(sorted(p.name for p in OUT.glob(f"{name}.*"))))
    write_scan(OUT / "dome_scan.stl")
    print("dome_scan: dome_scan.stl")
    return 0


if __name__ == "__main__":
    sys.exit(main())
