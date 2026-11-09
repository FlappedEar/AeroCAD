# SPDX-License-Identifier: GPL-3.0-or-later
"""CAD-95 diagnostic: shift pad by +/- 0.001 mm; NOT a production remedy."""
import argparse
import json
from pathlib import Path
import numpy as np
from OCP.BRep import BRep_Builder
from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut
from OCP.BRepBuilderAPI import BRepBuilderAPI_Transform
from OCP.BRepGProp import BRepGProp
from OCP.BRepTools import BRepTools
from OCP.GProp import GProp_GProps
from OCP.TopAbs import TopAbs_SOLID
from OCP.TopExp import TopExp_Explorer
from OCP.TopoDS import TopoDS_Iterator, TopoDS_Shape
from OCP.gp import gp_Trsf, gp_Vec
from diagnose_fin_brep import mesh
from diagnose_fin_mesh import controls, find_crossing
from diagnose_fin_stages import inspect

parser = argparse.ArgumentParser()
parser.add_argument('evidence', type=Path, help='extracted CAD-95-stage-diagnosis.zip')
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
controls()
results = []
for delta in (0.001, -0.001):
    compound = TopoDS_Shape()
    assert BRepTools.Read_s(compound, str(args.evidence / 'stages/fin-2-cut-inputs.brep'), BRep_Builder())
    iterator = TopoDS_Iterator(compound)
    shapes = []
    while iterator.More():
        shapes.append(iterator.Value())
        iterator.Next()
    transform = gp_Trsf()
    transform.SetTranslation(gp_Vec(delta, 0, 0))
    pad = BRepBuilderAPI_Transform(shapes[1], transform, True).Shape()
    cut = BRepAlgoAPI_Cut(shapes[0], pad)
    assert cut.IsDone()
    solids = []
    explorer = TopExp_Explorer(cut.Shape(), TopAbs_SOLID)
    while explorer.More():
        props = GProp_GProps()
        BRepGProp.VolumeProperties_s(explorer.Current(), props)
        solids.append((abs(props.Mass()), explorer.Current()))
        explorer.Next()
    solids.sort(key=lambda item: item[0], reverse=True)
    path = args.output / ('positive.brep' if delta > 0 else 'negative.brep')
    assert BRepTools.Write_s(solids[0][1], str(path))
    analysis = inspect(path)
    triangles, _, faces, linear = mesh(solids[0][1])
    crossing = find_crossing(triangles.astype(np.float32).astype(float))
    row = {'delta_mm': delta, 'solid_volumes_mm3': [s[0] for s in solids],
           'analysis': analysis, 'triangle_count': len(triangles), 'face_count': len(faces),
           'linear_deflection_mm': linear, 'crossing': crossing}
    results.append(row)
    (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(delta, 'faults', len(analysis['faults']), 'solids', row['solid_volumes_mm3'],
          'crossing', crossing.get('triangle_indices_zero_based'), flush=True)
