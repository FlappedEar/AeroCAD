// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Modeling.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepLib.hxx>
#include <ShapeFix_Solid.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepTools.hxx>
#include <gp_Ax2.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace aerocad::geometry {

namespace {

gp_Pnt toPnt(const core::Vec3& v)
{
    return {v.x, v.y, v.z};
}

core::Vec3 toVec(const gp_Pnt& p)
{
    return {p.X(), p.Y(), p.Z()};
}

const TopoDS_Shape& occ(const Shape& s, const char* what)
{
    if (s.isNull())
        throw ModelingError(std::string(what) + ": empty shape");
    return s.impl()->shape;
}

/// Runs an OCCT operation and converts its exceptions.
template <typename Fn>
auto guarded(const char* what, Fn&& fn)
{
    try {
        return fn();
    } catch (const Standard_Failure& e) {
        throw ModelingError(std::string(what) + " failed: " + e.GetMessageString());
    }
}

bool hasSubShapes(const TopoDS_Shape& s, TopAbs_ShapeEnum type)
{
    return TopExp_Explorer(s, type).More();
}

TopoDS_Shape unify(const TopoDS_Shape& s)
{
    ShapeUpgrade_UnifySameDomain unifier(s, true, true, false);
    unifier.Build();
    return unifier.Shape();
}

std::string surfaceType(const TopoDS_Face& f)
{
    switch (BRepAdaptor_Surface(f).GetType()) {
    case GeomAbs_Plane: return "plane";
    case GeomAbs_Cylinder: return "cylinder";
    case GeomAbs_Cone: return "cone";
    case GeomAbs_Sphere: return "sphere";
    case GeomAbs_Torus: return "torus";
    case GeomAbs_BSplineSurface:
    case GeomAbs_BezierSurface: return "bspline";
    default: return "other";
    }
}

std::string curveType(const TopoDS_Edge& e)
{
    switch (BRepAdaptor_Curve(e).GetType()) {
    case GeomAbs_Line: return "line";
    case GeomAbs_Circle: return "circle";
    case GeomAbs_Ellipse: return "ellipse";
    case GeomAbs_BSplineCurve:
    case GeomAbs_BezierCurve: return "bspline";
    default: return "other";
    }
}

TopTools_IndexedMapOfShape mapOf(const TopoDS_Shape& s, TopAbs_ShapeEnum type)
{
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(s, type, map);
    return map;
}

/// Outward normal of a face at its area centroid (projected to the surface).
core::Vec3 faceNormalAt(const TopoDS_Face& f, gp_Pnt& centerOut)
{
    GProp_GProps props;
    BRepGProp::SurfaceProperties(f, props);
    centerOut = props.CentreOfMass();
    BRepAdaptor_Surface surf(f);
    const double u = (surf.FirstUParameter() + surf.LastUParameter()) / 2;
    const double v = (surf.FirstVParameter() + surf.LastVParameter()) / 2;
    BRepLProp_SLProps lp(surf, u, v, 1, 1e-6);
    if (!lp.IsNormalDefined())
        return {};
    gp_Dir n = lp.Normal();
    if (f.Orientation() == TopAbs_REVERSED)
        n.Reverse();
    return {n.X(), n.Y(), n.Z()};
}

} // namespace

Shape meshToBrep(const core::TriangleMesh& mesh, std::size_t maxTriangles)
{
    if (mesh.isEmpty())
        throw ModelingError("Mesh to B-rep: the mesh is empty");
    if (mesh.triangleCount() > maxTriangles)
        throw ModelingError("Mesh to B-rep: " + std::to_string(mesh.triangleCount()) +
                            " triangles is too many for a faceted body (limit " + std::to_string(maxTriangles) +
                            "); simplify the mesh or select a region and fit a surface");
    return guarded("Mesh to B-rep", [&] {
        const double tol = std::max(mesh.bounds().diagonal() * 1e-6, 1e-6);
        BRepBuilderAPI_Sewing sewing(tol * 10);
        const auto& v = mesh.vertices();
        std::size_t faces = 0;
        for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
            const auto& tri = mesh.triangles()[t];
            if (mesh.faceNormal(t).length() < 0.5)
                continue; // degenerate
            BRepBuilderAPI_MakePolygon poly(toPnt(v[tri[0]]), toPnt(v[tri[1]]), toPnt(v[tri[2]]), true);
            if (!poly.IsDone())
                continue;
            BRepBuilderAPI_MakeFace face(poly.Wire(), true);
            if (!face.IsDone())
                continue;
            sewing.Add(face.Face());
            ++faces;
        }
        if (faces == 0)
            throw ModelingError("Mesh to B-rep: no valid triangles");
        sewing.Perform();
        TopoDS_Shape sewn = sewing.SewedShape();
        // Closed shells become solids, so booleans and volumes work. Open shells and
        // standalone faces are kept as they are, so no part of the mesh is dropped.
        BRep_Builder builder;
        TopoDS_Compound parts;
        builder.MakeCompound(parts);
        for (TopExp_Explorer ex(sewn, TopAbs_SHELL); ex.More(); ex.Next()) {
            const TopoDS_Shell& shell = TopoDS::Shell(ex.Current());
            if (BRep_Tool::IsClosed(shell)) {
                BRepBuilderAPI_MakeSolid mk(shell);
                if (mk.IsDone()) {
                    ShapeFix_Solid fix(mk.Solid());
                    fix.Perform();
                    builder.Add(parts, fix.Solid());
                    continue;
                }
            }
            builder.Add(parts, shell);
        }
        for (TopExp_Explorer ex(sewn, TopAbs_FACE, TopAbs_SHELL); ex.More(); ex.Next())
            builder.Add(parts, ex.Current());
        TopoDS_Shape result = parts;
        return wrapShape(unify(result));
    });
}

namespace {

Shape transformed(const Shape& shape, const gp_Trsf& t, const char* what)
{
    const TopoDS_Shape& s = occ(shape, what);
    return guarded(what, [&] {
        BRepBuilderAPI_Transform tr(s, t, true);
        if (!tr.IsDone())
            throw ModelingError(std::string(what) + " failed");
        return wrapShape(tr.Shape());
    });
}

gp_Dir dirOf(const core::Vec3& v, const char* what)
{
    const core::Vec3 n = v.normalized();
    if (n.length() < 0.5)
        throw ModelingError(std::string(what) + ": zero direction");
    return {n.x, n.y, n.z};
}

} // namespace

Shape mirrored(const Shape& shape, const core::Vec3& planeOrigin, const core::Vec3& planeNormal)
{
    gp_Trsf t;
    t.SetMirror(gp_Ax2(toPnt(planeOrigin), dirOf(planeNormal, "Mirror")));
    return transformed(shape, t, "Mirror");
}

Shape rotated(const Shape& shape, const core::Vec3& axisOrigin, const core::Vec3& axisDirection, double angleDegrees)
{
    gp_Trsf t;
    t.SetRotation(gp_Ax1(toPnt(axisOrigin), dirOf(axisDirection, "Rotate")), angleDegrees * std::numbers::pi / 180.0);
    return transformed(shape, t, "Rotate");
}

Shape loft(const std::vector<Shape>& sections, bool solid, bool ruled)
{
    if (sections.size() < 2)
        throw ModelingError("Loft needs at least two profiles");
    return guarded("Loft", [&] {
        BRepOffsetAPI_ThruSections thru(solid, ruled, 1e-6);
        for (const auto& sec : sections) {
            const TopoDS_Shape& s = occ(sec, "Loft");
            TopExp_Explorer ex(s, TopAbs_FACE);
            if (!ex.More())
                throw ModelingError("Loft: every section must be a closed profile");
            thru.AddWire(BRepTools::OuterWire(TopoDS::Face(ex.Current())));
        }
        thru.CheckCompatibility(true);
        thru.Build();
        if (!thru.IsDone())
            throw ModelingError("Loft failed");
        return wrapShape(thru.Shape());
    });
}

Shape sweep(const Shape& profileFaces, const Shape& pathEdges)
{
    const TopoDS_Shape& prof = occ(profileFaces, "Sweep");
    const TopoDS_Shape& path = occ(pathEdges, "Sweep");
    if (!hasSubShapes(prof, TopAbs_FACE))
        throw ModelingError("Sweep: no profile faces");
    return guarded("Sweep", [&] {
        BRepBuilderAPI_MakeWire wire;
        TopTools_ListOfShape edgeList;
        for (TopExp_Explorer ex(path, TopAbs_EDGE); ex.More(); ex.Next())
            edgeList.Append(ex.Current());
        if (edgeList.IsEmpty())
            throw ModelingError("Sweep: the path has no curves");
        wire.Add(edgeList);
        if (!wire.IsDone())
            throw ModelingError("Sweep: the path curves are not connected");
        BRep_Builder builder;
        TopoDS_Compound comp;
        builder.MakeCompound(comp);
        for (TopExp_Explorer ex(prof, TopAbs_FACE); ex.More(); ex.Next()) {
            BRepOffsetAPI_MakePipe pipe(wire.Wire(), ex.Current());
            pipe.Build();
            if (!pipe.IsDone())
                throw ModelingError("Sweep failed");
            builder.Add(comp, pipe.Shape());
        }
        return wrapShape(comp);
    });
}

Shape makeCompound(const std::vector<Shape>& shapes)
{
    BRep_Builder builder;
    TopoDS_Compound comp;
    builder.MakeCompound(comp);
    bool any = false;
    for (const auto& s : shapes) {
        if (!s.isNull()) {
            builder.Add(comp, s.impl()->shape);
            any = true;
        }
    }
    return any ? wrapShape(comp) : Shape{};
}

Shape extrude(const Shape& faces, const core::Vec3& direction, double distance, double backDistance)
{
    const TopoDS_Shape& s = occ(faces, "Extrude");
    if (!hasSubShapes(s, TopAbs_FACE))
        throw ModelingError("Extrude: no profile faces");
    const core::Vec3 dir = direction.normalized();
    if (dir.length() < 0.5)
        throw ModelingError("Extrude: zero direction");
    const double total = distance + backDistance;
    if (std::abs(total) < 1e-9)
        throw ModelingError("Extrude: zero distance");
    return guarded("Extrude", [&] {
        TopoDS_Shape base = s;
        if (backDistance != 0.0) {
            gp_Trsf t;
            t.SetTranslation(gp_Vec(-dir.x * backDistance, -dir.y * backDistance, -dir.z * backDistance));
            base = BRepBuilderAPI_Transform(s, t, true).Shape();
        }
        BRepPrimAPI_MakePrism prism(base, gp_Vec(dir.x * total, dir.y * total, dir.z * total), true);
        if (!prism.IsDone())
            throw ModelingError("Extrude failed");
        return wrapShape(prism.Shape());
    });
}

Shape revolve(const Shape& faces, const core::Vec3& axisOrigin, const core::Vec3& axisDirection, double angleDegrees)
{
    const TopoDS_Shape& s = occ(faces, "Revolve");
    if (!hasSubShapes(s, TopAbs_FACE))
        throw ModelingError("Revolve: no profile faces");
    const core::Vec3 dir = axisDirection.normalized();
    if (dir.length() < 0.5)
        throw ModelingError("Revolve: zero axis direction");
    if (std::abs(angleDegrees) < 1e-9 || std::abs(angleDegrees) > 360.0)
        throw ModelingError("Revolve: angle must be in (0, 360] degrees");
    return guarded("Revolve", [&] {
        const gp_Ax1 axis(toPnt(axisOrigin), gp_Dir(dir.x, dir.y, dir.z));
        BRepPrimAPI_MakeRevol revol(s, axis, angleDegrees * std::numbers::pi / 180.0, true);
        if (!revol.IsDone())
            throw ModelingError("Revolve failed");
        return wrapShape(revol.Shape());
    });
}

Shape boolean(const Shape& a, const Shape& b, BooleanOp op)
{
    const TopoDS_Shape& sa = occ(a, "Boolean");
    const TopoDS_Shape& sb = occ(b, "Boolean");
    return guarded("Boolean", [&] {
        TopoDS_Shape result;
        auto run = [&](auto&& algo) {
            algo.SetRunParallel(false);
            algo.Build();
            if (!algo.IsDone() || algo.HasErrors())
                throw ModelingError("Boolean operation failed");
            result = algo.Shape();
        };
        TopTools_ListOfShape args, tools;
        args.Append(sa);
        tools.Append(sb);
        switch (op) {
        case BooleanOp::Union: {
            BRepAlgoAPI_Fuse algo;
            algo.SetArguments(args);
            algo.SetTools(tools);
            run(algo);
            break;
        }
        case BooleanOp::Cut: {
            BRepAlgoAPI_Cut algo;
            algo.SetArguments(args);
            algo.SetTools(tools);
            run(algo);
            break;
        }
        case BooleanOp::Intersect: {
            BRepAlgoAPI_Common algo;
            algo.SetArguments(args);
            algo.SetTools(tools);
            run(algo);
            break;
        }
        }
        if (result.IsNull() || !hasSubShapes(result, TopAbs_FACE))
            return Shape{};
        return wrapShape(unify(result));
    });
}

Shape fillet(const Shape& shape, const std::vector<std::size_t>& edgeIndices, double radius)
{
    const TopoDS_Shape& s = occ(shape, "Fillet");
    if (radius <= 0.0)
        throw ModelingError("Fillet: radius must be positive");
    if (edgeIndices.empty())
        throw ModelingError("Fillet: no edges selected");
    return guarded("Fillet", [&] {
        const auto map = mapOf(s, TopAbs_EDGE);
        BRepFilletAPI_MakeFillet mk(s);
        for (const std::size_t i : edgeIndices) {
            if (i >= static_cast<std::size_t>(map.Extent()))
                throw ModelingError("Fillet: no edge " + std::to_string(i));
            mk.Add(radius, TopoDS::Edge(map(static_cast<int>(i) + 1)));
        }
        mk.Build();
        if (!mk.IsDone())
            throw ModelingError("Fillet failed (radius too large for the selected edges?)");
        return wrapShape(mk.Shape());
    });
}

Shape chamfer(const Shape& shape, const std::vector<std::size_t>& edgeIndices, double distance)
{
    const TopoDS_Shape& s = occ(shape, "Chamfer");
    if (distance <= 0.0)
        throw ModelingError("Chamfer: distance must be positive");
    if (edgeIndices.empty())
        throw ModelingError("Chamfer: no edges selected");
    return guarded("Chamfer", [&] {
        const auto map = mapOf(s, TopAbs_EDGE);
        BRepFilletAPI_MakeChamfer mk(s);
        for (const std::size_t i : edgeIndices) {
            if (i >= static_cast<std::size_t>(map.Extent()))
                throw ModelingError("Chamfer: no edge " + std::to_string(i));
            mk.Add(distance, TopoDS::Edge(map(static_cast<int>(i) + 1)));
        }
        mk.Build();
        if (!mk.IsDone())
            throw ModelingError("Chamfer failed (distance too large for the selected edges?)");
        return wrapShape(mk.Shape());
    });
}

Shape shell(const Shape& shape, const std::vector<std::size_t>& faceIndices, double thickness)
{
    const TopoDS_Shape& s = occ(shape, "Shell");
    if (thickness <= 0.0)
        throw ModelingError("Shell: thickness must be positive");
    return guarded("Shell", [&] {
        const auto map = mapOf(s, TopAbs_FACE);
        TopTools_ListOfShape removed;
        for (const std::size_t i : faceIndices) {
            if (i >= static_cast<std::size_t>(map.Extent()))
                throw ModelingError("Shell: no face " + std::to_string(i));
            removed.Append(map(static_cast<int>(i) + 1));
        }
        BRepOffsetAPI_MakeThickSolid mk;
        mk.MakeThickSolidByJoin(s, removed, -thickness, 1e-3);
        if (!mk.IsDone())
            throw ModelingError("Shell failed (thickness too large?)");
        return wrapShape(mk.Shape());
    });
}

bool isValid(const Shape& shape)
{
    if (shape.isNull())
        return false;
    return BRepCheck_Analyzer(shape.impl()->shape).IsValid();
}

bool boundsOverlap(const Shape& a, const Shape& b)
{
    if (a.isNull() || b.isNull())
        return false;
    Bnd_Box ba, bb;
    BRepBndLib::Add(a.impl()->shape, ba);
    BRepBndLib::Add(b.impl()->shape, bb);
    ba.Enlarge(1e-4);
    bb.Enlarge(1e-4);
    return !ba.IsOut(bb);
}

bool touches(const Shape& a, const Shape& b, double tolerance)
{
    if (!boundsOverlap(a, b))
        return false;
    return guarded("Contact check", [&] {
        // Distance between solids is 0 when one is inside the other, so this also covers
        // containment.
        BRepExtrema_DistShapeShape dist(a.impl()->shape, b.impl()->shape);
        return dist.IsDone() && dist.Value() <= tolerance;
    });
}

std::vector<FaceInfo> faces(const Shape& shape)
{
    std::vector<FaceInfo> out;
    if (shape.isNull())
        return out;
    const auto map = mapOf(shape.impl()->shape, TopAbs_FACE);
    for (int i = 1; i <= map.Extent(); ++i) {
        const TopoDS_Face& f = TopoDS::Face(map(i));
        FaceInfo info;
        info.index = static_cast<std::size_t>(i - 1);
        info.type = surfaceType(f);
        GProp_GProps props;
        BRepGProp::SurfaceProperties(f, props);
        info.area = props.Mass();
        gp_Pnt c;
        info.normal = faceNormalAt(f, c);
        info.center = toVec(c);
        out.push_back(info);
    }
    return out;
}

std::vector<EdgeInfo> edges(const Shape& shape)
{
    std::vector<EdgeInfo> out;
    if (shape.isNull())
        return out;
    const auto map = mapOf(shape.impl()->shape, TopAbs_EDGE);
    for (int i = 1; i <= map.Extent(); ++i) {
        const TopoDS_Edge& e = TopoDS::Edge(map(i));
        if (BRep_Tool::Degenerated(e))
            continue;
        EdgeInfo info;
        info.index = static_cast<std::size_t>(i - 1);
        info.type = curveType(e);
        BRepAdaptor_Curve c(e);
        info.length = GCPnts_AbscissaPoint::Length(c);
        info.start = toVec(c.Value(c.FirstParameter()));
        info.end = toVec(c.Value(c.LastParameter()));
        info.middle = toVec(c.Value((c.FirstParameter() + c.LastParameter()) / 2));
        out.push_back(info);
    }
    return out;
}

namespace {

std::vector<double> distancesTo(const Shape& shape, const core::Vec3& point, TopAbs_ShapeEnum type)
{
    std::vector<double> out;
    if (shape.isNull())
        return out;
    const auto map = mapOf(shape.impl()->shape, type);
    const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(toPnt(point)).Vertex();
    out.assign(static_cast<std::size_t>(map.Extent()), std::numeric_limits<double>::infinity());
    for (int i = 1; i <= map.Extent(); ++i) {
        if (type == TopAbs_EDGE && BRep_Tool::Degenerated(TopoDS::Edge(map(i))))
            continue;
        BRepExtrema_DistShapeShape dist(v, map(i));
        if (dist.IsDone())
            out[static_cast<std::size_t>(i - 1)] = dist.Value();
    }
    return out;
}

std::optional<std::size_t> nearest(const Shape& shape, const core::Vec3& point, TopAbs_ShapeEnum type)
{
    if (shape.isNull())
        return std::nullopt;
    const auto map = mapOf(shape.impl()->shape, type);
    const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(toPnt(point)).Vertex();
    std::optional<std::size_t> best;
    double bestDist = 0.0;
    for (int i = 1; i <= map.Extent(); ++i) {
        if (type == TopAbs_EDGE && BRep_Tool::Degenerated(TopoDS::Edge(map(i))))
            continue;
        BRepExtrema_DistShapeShape dist(v, map(i));
        if (!dist.IsDone())
            continue;
        if (!best || dist.Value() < bestDist - 1e-9) {
            best = static_cast<std::size_t>(i - 1);
            bestDist = dist.Value();
        }
    }
    return best;
}

} // namespace

std::optional<std::size_t> nearestEdge(const Shape& shape, const core::Vec3& point)
{
    return guarded("Edge lookup", [&] { return nearest(shape, point, TopAbs_EDGE); });
}

std::optional<std::size_t> nearestFace(const Shape& shape, const core::Vec3& point)
{
    return guarded("Face lookup", [&] { return nearest(shape, point, TopAbs_FACE); });
}

std::vector<double> edgeDistances(const Shape& shape, const core::Vec3& point)
{
    return guarded("Edge lookup", [&] { return distancesTo(shape, point, TopAbs_EDGE); });
}

std::vector<double> faceDistances(const Shape& shape, const core::Vec3& point)
{
    return guarded("Face lookup", [&] { return distancesTo(shape, point, TopAbs_FACE); });
}

std::optional<Plane> facePlane(const Shape& shape, std::size_t faceIndex)
{
    if (shape.isNull())
        return std::nullopt;
    const auto map = mapOf(shape.impl()->shape, TopAbs_FACE);
    if (faceIndex >= static_cast<std::size_t>(map.Extent()))
        return std::nullopt;
    const TopoDS_Face& f = TopoDS::Face(map(static_cast<int>(faceIndex) + 1));
    BRepAdaptor_Surface surf(f);
    if (surf.GetType() != GeomAbs_Plane)
        return std::nullopt;
    const gp_Pln pln = surf.Plane();
    gp_Dir n = pln.Axis().Direction();
    gp_Dir x = pln.XAxis().Direction();
    if (f.Orientation() == TopAbs_REVERSED) {
        n.Reverse();
        x.Reverse(); // keep a right-handed frame on the flipped plane
    }
    // Project the face centroid onto the plane as the sketch origin.
    GProp_GProps props;
    BRepGProp::SurfaceProperties(f, props);
    Plane p;
    p.origin = toVec(props.CentreOfMass());
    p.normal = {n.X(), n.Y(), n.Z()};
    p.xDir = {x.X(), x.Y(), x.Z()};
    return p.normalized();
}

} // namespace aerocad::geometry
