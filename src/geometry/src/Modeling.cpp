// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Modeling.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
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
