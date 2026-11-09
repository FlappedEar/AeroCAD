# SPDX-License-Identifier: GPL-3.0-or-later
"""Map CAD-95 saved STL witnesses to the unchanged saved project BREP.

Requires NumPy and cadquery-ocp. Replays the application's export tessellation
parameters (0.0002 relative linear / 0.15 angular), retaining face provenance.
This is a diagnostic, not a production fix or full mesh acceptance gate.
"""
import argparse
import io
import json
from pathlib import Path
import zipfile

import numpy as np
from OCP.Bnd import Bnd_Box
from OCP.BOPAlgo import BOPAlgo_ArgumentAnalyzer
from OCP.BRep import BRep_Builder, BRep_Tool
from OCP.BRepAdaptor import BRepAdaptor_Surface
from OCP.BRepAdaptor import BRepAdaptor_Curve
from OCP.BRepAlgoAPI import BRepAlgoAPI_Section
from OCP.BRepBndLib import BRepBndLib
from OCP.BRepBuilderAPI import BRepBuilderAPI_MakeVertex
from OCP.BRepCheck import BRepCheck_Analyzer
from OCP.BRepClass import BRepClass_FaceClassifier
from OCP.BRepExtrema import BRepExtrema_DistShapeShape
from OCP.BRepGProp import BRepGProp
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.BRepTools import BRepTools
from OCP.GeomAPI import GeomAPI_ProjectPointOnSurf
from OCP.GProp import GProp_GProps
from OCP.TopAbs import TopAbs_EDGE, TopAbs_FACE, TopAbs_REVERSED, TopAbs_VERTEX
from OCP.TopExp import TopExp, TopExp_Explorer
from OCP.TopLoc import TopLoc_Location
from OCP.TopoDS import TopoDS, TopoDS_Shape
from OCP.TopTools import TopTools_IndexedMapOfShape
from OCP.gp import gp_Pnt, gp_Pnt2d

from diagnose_fin_mesh import certify, read_stl


def shape_map(shape, kind):
    result = TopTools_IndexedMapOfShape()
    TopExp.MapShapes_s(shape, kind, result)
    return result


def key(triangle):
    return tuple(sorted(tuple(float(x) for x in p) for p in np.asarray(triangle, dtype=np.float32)))


def mesh(shape):
    bounds = Bnd_Box()
    BRepBndLib.Add_s(shape, bounds)
    box = bounds.Get()
    linear = max(float(np.linalg.norm(np.asarray(box[3:]) - box[:3])) * .0002, .0001)
    mesher = BRepMesh_IncrementalMesh(shape, linear, False, .15, True)
    if not mesher.IsDone():
        raise RuntimeError('OCCT meshing did not finish')
    triangles, provenance, faces = [], [], []
    explorer = TopExp_Explorer(shape, TopAbs_FACE)
    while explorer.More():
        face = TopoDS.Face_s(explorer.Current())
        faces.append(face)
        loc = TopLoc_Location()
        poly = BRep_Tool.Triangulation_s(face, loc)
        if poly:
            nodes = [poly.Node(i).Transformed(loc.Transformation()).Coord() for i in range(1, poly.NbNodes() + 1)]
            for i in range(1, poly.NbTriangles() + 1):
                ids = list(poly.Triangle(i).Get())
                if face.Orientation() == TopAbs_REVERSED:
                    ids[1], ids[2] = ids[2], ids[1]
                points = [nodes[j - 1] for j in ids]
                if len(set(points)) < 3:
                    continue
                triangles.append(points)
                provenance.append(len(faces))
        explorer.Next()
    return np.asarray(triangles), provenance, faces, linear


def point_on_face(face, point):
    projector = GeomAPI_ProjectPointOnSurf(gp_Pnt(*point), BRep_Tool.Surface_s(face))
    if not projector.NbPoints():
        return {'projection': 'unavailable'}
    uv = projector.LowerDistanceParameters()
    return {'surface_type': str(BRepAdaptor_Surface(face).GetType()),
            'surface_distance_mm': projector.LowerDistance(), 'uv': list(uv),
            'trim_class_at_1e_7': str(BRepClass_FaceClassifier(face, gp_Pnt2d(*uv), 1e-7).State())}


def inspect_section(shape, face_ids):
    faces = shape_map(shape, TopAbs_FACE)
    a, b = [TopoDS.Face_s(faces.FindKey(i)) for i in face_ids]
    ae, be = shape_map(a, TopAbs_EDGE), shape_map(b, TopAbs_EDGE)
    shared = [ae.FindKey(i) for i in range(1, ae.Extent() + 1) if be.Contains(ae.FindKey(i))]
    section = BRepAlgoAPI_Section(a, b, False)
    section.Build()
    out = {'faces_one_based': face_ids, 'section_done': section.IsDone(),
           'shared_edges': len(shared), 'section_edges': []}
    if not section.IsDone():
        return out
    edges = shape_map(section.Shape(), TopAbs_EDGE)
    for i in range(1, edges.Extent() + 1):
        edge = TopoDS.Edge_s(edges.FindKey(i))
        curve = BRepAdaptor_Curve(edge)
        properties = GProp_GProps()
        BRepGProp.LinearProperties_s(edge, properties)
        distances, samples = [], []
        if shared:
            for k in range(21):
                point = curve.Value(curve.FirstParameter() + (curve.LastParameter() - curve.FirstParameter()) * k / 20)
                vertex = BRepBuilderAPI_MakeVertex(point).Vertex()
                samples.append(point.Coord())
                distances.append(min(BRepExtrema_DistShapeShape(vertex, existing).Value() for existing in shared))
        record = {'length_mm': properties.Mass(),
                  'same_as_shared_edge': any(edge.IsSame(existing) for existing in shared),
                  'maximum_sampled_distance_to_shared_edge_mm': max(distances) if distances else None}
        if distances:
            point = samples[int(np.argmax(distances))]
            record['farthest_sample'] = list(point)
            record['farthest_sample_on_faces'] = [point_on_face(face, point) for face in (a, b)]
        out['section_edges'].append(record)
    return out


def inspect_shape(shape, witness, original):
    maps = {typ: shape_map(shape, typ) for typ in (TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX)}
    analyzer = BOPAlgo_ArgumentAnalyzer()
    analyzer.SetShape1(shape)
    analyzer.SelfInterMode = True
    analyzer.Perform()
    faults = []
    for result in analyzer.GetCheckResult():
        shapes = []
        for item in result.GetFaultyShapes1():
            kind = item.ShapeType()
            record = {'type': str(kind), 'type_index_one_based': maps[kind].FindIndex(item)}
            if kind == TopAbs_EDGE:
                record['tolerance_mm'] = BRep_Tool.Tolerance_s(TopoDS.Edge_s(item))
                record['adjacent_faces'] = [i for i in range(1, maps[TopAbs_FACE].Extent() + 1)
                                            if shape_map(maps[TopAbs_FACE].FindKey(i), TopAbs_EDGE).Contains(item)]
            if kind == TopAbs_FACE:
                record['surface_type'] = str(BRepAdaptor_Surface(TopoDS.Face_s(item)).GetType())
            shapes.append(record)
        faults.append({'status': str(result.GetCheckStatus()), 'shapes': shapes})

    triangles, provenance, faces, linear = mesh(shape)
    lookup = {}
    for index, triangle in enumerate(triangles):
        lookup.setdefault(key(triangle), []).append(index)
    ids = witness['triangle_indices_zero_based']
    candidates = [lookup.get(key(original[i]), []) for i in ids]
    result = {'brepcheck_valid': BRepCheck_Analyzer(shape).IsValid(),
              'self_interference_checker_has_errors': analyzer.HasErrors(),
              'self_interference_faults': faults, 'source_faces': len(faces),
              'remeshed_triangles': len(triangles), 'linear_deflection_mm': linear,
              'all_stl_coordinates_equal_float32_source_mesh': bool(np.array_equal(triangles.astype(np.float32), original)),
              'original_witness_indices': ids, 'matching_remeshed_indices': candidates}
    if all(len(c) == 1 for c in candidates):
        a, b = (c[0] for c in candidates)
        face_ids = [provenance[a], provenance[b]]
        result['witness_faces_one_based'] = face_ids
        result['double_precision_source_pair_certificate'] = certify(triangles[a], triangles[b])
        c = witness['certificate']
        midpoint = [(float(x) + float(y)) / 2 for x, y in zip(c['start'], c['end'])]
        result['stl_crossing_midpoint_on_source_faces'] = [point_on_face(faces[i - 1], midpoint) for i in face_ids]
        edges = [shape_map(faces[i - 1], TopAbs_EDGE) for i in face_ids]
        result['shared_source_edges'] = [{'index_one_based': maps[TopAbs_EDGE].FindIndex(edges[0].FindKey(i)),
                                          'tolerance_mm': BRep_Tool.Tolerance_s(TopoDS.Edge_s(edges[0].FindKey(i)))}
                                         for i in range(1, edges[0].Extent() + 1) if edges[1].Contains(edges[0].FindKey(i))]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=Path)
    parser.add_argument('export_folder', type=Path)
    parser.add_argument('certificates', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--body', help='Optional exact body name for a bounded single-body diagnosis')
    parser.add_argument('--section-faces', type=int, nargs=2, help='Additionally section one pair of one-based source face indices')
    args = parser.parse_args()
    certificates = {Path(p['file']).stem: p for p in json.loads(args.certificates.read_text())['parts']}
    out = {'issue': 'CAD-95', 'source': 'Original saved project BREP; inputs unchanged', 'parts': []}
    with zipfile.ZipFile(args.project) as archive:
        manifest = json.loads(archive.read('manifest.json'))
        for record in manifest['timelineResults']:
            for body in record['bodies']:
                if args.body and body['name'] != args.body:
                    continue
                shape = TopoDS_Shape()
                BRepTools.Read_s(shape, io.BytesIO(archive.read(body['brep'])), BRep_Builder())
                original = read_stl(args.export_folder / (body['name'] + '.stl'))
                diagnosed = {'name': body['name'], 'brep_entry': body['brep'],
                             **inspect_shape(shape, certificates[body['name']], original)}
                if args.section_faces:
                    diagnosed['face_section'] = inspect_section(shape, args.section_faces)
                out['parts'].append(diagnosed)
                args.output.write_text(json.dumps(out, indent=2) + '\n')
                print('Diagnosed ' + body['name'], flush=True)


if __name__ == '__main__':
    main()
