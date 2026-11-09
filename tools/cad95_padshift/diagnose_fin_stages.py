#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect captured CAD-95 BREP stages; diagnostics only, not a production validator."""
import argparse
import json
from pathlib import Path

from OCP.BOPAlgo import BOPAlgo_ArgumentAnalyzer
from OCP.BRep import BRep_Builder
from OCP.BRepCheck import BRepCheck_Analyzer
from OCP.BRepGProp import BRepGProp
from OCP.BRepTools import BRepTools
from OCP.GProp import GProp_GProps
from OCP.TopAbs import TopAbs_EDGE, TopAbs_FACE, TopAbs_VERTEX
from OCP.TopExp import TopExp
from OCP.TopoDS import TopoDS_Shape
from OCP.TopTools import TopTools_IndexedMapOfShape


def inspect(path):
    shape = TopoDS_Shape()
    if not BRepTools.Read_s(shape, str(path), BRep_Builder()):
        raise RuntimeError(f'Could not read {path}')
    maps = {}
    for kind in (TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX):
        maps[kind] = TopTools_IndexedMapOfShape()
        TopExp.MapShapes_s(shape, kind, maps[kind])
    analyzer = BOPAlgo_ArgumentAnalyzer()
    analyzer.SetShape1(shape)
    analyzer.SelfInterMode = True
    analyzer.Perform()
    faults = []
    for result in analyzer.GetCheckResult():
        shapes = []
        for item in result.GetFaultyShapes1():
            kind = item.ShapeType()
            shapes.append({'type': str(kind), 'index': maps[kind].FindIndex(item) if kind in maps else None})
        faults.append({'status': str(result.GetCheckStatus()), 'shapes': shapes})
    props = GProp_GProps()
    BRepGProp.VolumeProperties_s(shape, props)
    return {'file': path.name, 'valid_brepcheck': BRepCheck_Analyzer(shape).IsValid(),
            'volume_mm3': props.Mass(), 'faces': maps[TopAbs_FACE].Extent(),
            'checker_errors': analyzer.HasErrors(), 'faults': faults}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    files = sorted(args.directory.glob('fin-*.brep'))
    if not files:
        raise SystemExit('No captured stages found')
    results = []
    for path in files:
        result = inspect(path)
        results.append(result)
        print(path.name, 'BRepCheck', result['valid_brepcheck'], 'faults', len(result['faults']), flush=True)
    args.output.write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
