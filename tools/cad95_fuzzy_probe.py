import json, sys
from pathlib import Path
from OCP.BRep import BRep_Builder
from OCP.BRepTools import BRepTools
from OCP.BRepAlgoAPI import BRepAlgoAPI_Cut
from OCP.BRepGProp import BRepGProp
from OCP.GProp import GProp_GProps
from OCP.TopoDS import TopoDS_Shape, TopoDS_Iterator
from OCP.TopTools import TopTools_ListOfShape
sys.path.insert(0, '/workspace/scratch/ac02f05ba598/worktrees/CAD-95-stage-probe/tools')
from diagnose_fin_stages import inspect
root=Path('/workspace/scratch/ac02f05ba598/cad95-stage-evidence')
out=root/'fuzzy-probe'
out.mkdir(exist_ok=True)
results=[]
for tol in (0.0, 1e-7, 1e-6, 1e-5, 1e-4, 1e-3):
    c=TopoDS_Shape(); assert BRepTools.Read_s(c,str(root/'stages-v4/fin-2-cut-inputs.brep'),BRep_Builder())
    it=TopoDS_Iterator(c); shapes=[]
    while it.More():
        shapes.append(it.Value()); it.Next()
    args,tools=TopTools_ListOfShape(),TopTools_ListOfShape(); args.Append(shapes[0]); tools.Append(shapes[1])
    cut=BRepAlgoAPI_Cut(); cut.SetArguments(args); cut.SetTools(tools); cut.SetRunParallel(False); cut.SetFuzzyValue(tol); cut.Build()
    item={'fuzzy_mm':tol,'done':cut.IsDone()}
    if cut.IsDone():
        shape=cut.Shape(); g=GProp_GProps(); BRepGProp.VolumeProperties_s(shape,g); item['volume_mm3']=g.Mass()
        path=out/('fuzzy-'+str(tol)+'.brep'); BRepTools.Write_s(shape,str(path)); item['analysis']=inspect(path)
    results.append(item); (out/'results.json').write_text(json.dumps(results,indent=2)+'\n'); print(tol,item['done'],len(item.get('analysis',{}).get('faults',[])),flush=True)
