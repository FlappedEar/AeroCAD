import json, sys, time
from pathlib import Path
from OCP.BRep import BRep_Builder
from OCP.BRepAlgoAPI import BRepAlgoAPI_Fuse
from OCP.BRepPrimAPI import BRepPrimAPI_MakeBox, BRepPrimAPI_MakeSphere
from OCP.BRepTools import BRepTools
from OCP.BOPAlgo import BOPAlgo_ArgumentAnalyzer
from OCP.gp import gp_Pnt
root=Path('/workspace/scratch/ac02f05ba598/cad95-stage-evidence')
out=root/'guard-probe'; out.mkdir(exist_ok=True)
shapes=[]
for i in range(4):
    for stage in ('fillet','result'):
        from OCP.TopoDS import TopoDS_Shape
        s=TopoDS_Shape(); assert BRepTools.Read_s(s,str(root/f'stages-v4/fin-{i}-{stage}.brep'),BRep_Builder()); shapes.append((f'fin-{i}-{stage}',s))
shapes.append(('box',BRepPrimAPI_MakeBox(100,80,60).Shape()))
shapes.append(('sphere',BRepPrimAPI_MakeSphere(40).Shape()))
overlap=BRepAlgoAPI_Fuse(BRepPrimAPI_MakeBox(100,80,60).Shape(),BRepPrimAPI_MakeSphere(gp_Pnt(0,0,0),40).Shape()); shapes.append(('fused-box-sphere',overlap.Shape()))
rows=[]
for label,shape in shapes:
    a=BOPAlgo_ArgumentAnalyzer(); a.SetShape1(shape); a.SelfInterMode=True
    start=time.perf_counter(); a.Perform(); seconds=time.perf_counter()-start
    row={'fixture':label,'seconds':seconds,'has_errors':a.HasErrors(),'faults':len(list(a.GetCheckResult()))}
    rows.append(row); print(json.dumps(row),flush=True); (out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
