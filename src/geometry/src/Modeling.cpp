// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Modeling.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBndLib.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepLib.hxx>
#include <ShapeFix_Solid.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Pnt2d.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
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
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>
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
    // Outer boundary of every section, plus its holes. Rings (one hole in every section)
    // are lofted as outer loft minus inner loft; other hole layouts cannot be matched
    // between sections reliably and are rejected instead of being silently filled.
    std::vector<TopoDS_Wire> outer;
    std::vector<std::vector<TopoDS_Wire>> holes;
    for (const auto& sec : sections) {
        const TopoDS_Shape& s = occ(sec, "Loft");
        TopExp_Explorer ex(s, TopAbs_FACE);
        if (!ex.More())
            throw ModelingError("Loft: every section must be a closed profile");
        const TopoDS_Face face = TopoDS::Face(ex.Current());
        ex.Next();
        if (ex.More())
            throw ModelingError("Loft: every section must be a single profile");
        const TopoDS_Wire out = BRepTools::OuterWire(face);
        outer.push_back(out);
        std::vector<TopoDS_Wire> inner;
        for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
            if (!w.Current().IsSame(out))
                inner.push_back(TopoDS::Wire(w.Current()));
        }
        holes.push_back(std::move(inner));
    }
    const std::size_t holeCount = holes.front().size();
    for (const auto& h : holes) {
        if (h.size() != holeCount)
            throw ModelingError("Loft: all profiles must have the same number of holes");
    }
    if (holeCount > 1)
        throw ModelingError("Loft: profiles with more than one hole are not supported yet");
    if (holeCount == 1 && !solid)
        throw ModelingError("Loft: profiles with a hole need a solid loft");

    auto thruSections = [&](const std::vector<TopoDS_Wire>& wires) {
        return guarded("Loft", [&] {
            BRepOffsetAPI_ThruSections thru(solid, ruled, 1e-6);
            for (const auto& w : wires)
                thru.AddWire(w);
            thru.CheckCompatibility(true);
            thru.Build();
            if (!thru.IsDone())
                throw ModelingError("Loft failed");
            return wrapShape(thru.Shape());
        });
    };
    Shape result = thruSections(outer);
    if (holeCount == 1) {
        std::vector<TopoDS_Wire> inner;
        for (const auto& h : holes)
            inner.push_back(h.front());
        result = boolean(result, thruSections(inner), BooleanOp::Cut);
    }
    return result;
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

Shape extrudeToObject(const Shape& faces, const core::Vec3& direction, const Shape& target,
                      const std::vector<std::size_t>& targetFaces, double reach)
{
    const TopoDS_Shape& s = occ(faces, "Extrude");
    if (!hasSubShapes(s, TopAbs_FACE))
        throw ModelingError("Extrude: no profile faces");
    const TopoDS_Shape& t = occ(target, "Extrude to object");
    const core::Vec3 dir = direction.normalized();
    if (dir.length() < 0.5)
        throw ModelingError("Extrude: zero direction");
    return guarded("Extrude to object", [&] {
        // The cutting tool: the picked faces, or the whole target.
        TopoDS_Shape tool = t;
        if (!targetFaces.empty()) {
            const auto map = mapOf(t, TopAbs_FACE);
            TopoDS_Compound c;
            BRep_Builder b;
            b.MakeCompound(c);
            for (const std::size_t i : targetFaces) {
                if (i >= static_cast<std::size_t>(map.Extent()))
                    throw ModelingError("Extrude to object: no face " + std::to_string(i));
                b.Add(c, map(static_cast<int>(i) + 1));
            }
            tool = c;
        }
        double length = reach;
        if (length <= 0.0) {
            Bnd_Box box;
            BRepBndLib::Add(s, box);
            BRepBndLib::Add(tool, box);
            length = 2.0 * std::sqrt(box.SquareExtent()) + 1.0;
        }
        BRepPrimAPI_MakePrism prism(s, gp_Vec(dir.x * length, dir.y * length, dir.z * length), true);
        if (!prism.IsDone())
            throw ModelingError("Extrude failed");
        GProp_GProps whole;
        BRepGProp::VolumeProperties(prism.Shape(), whole);

        TopTools_ListOfShape args, tools;
        args.Append(prism.Shape());
        tools.Append(tool);
        BRepAlgoAPI_Splitter splitter;
        splitter.SetArguments(args);
        splitter.SetTools(tools);
        splitter.SetRunParallel(false);
        splitter.Build();
        if (!splitter.IsDone() || splitter.HasErrors())
            throw ModelingError("Extrude to object: cutting at the object failed");

        // Keep the pieces that start on the profile: what lies before the object. A kept
        // piece that still reaches the far end went past (or around) the object.
        const double tol = 1e-6 * std::max(1.0, length);
        gp_Trsf toFar;
        toFar.SetTranslation(gp_Vec(dir.x * length, dir.y * length, dir.z * length));
        const TopoDS_Shape farCap = BRepBuilderAPI_Transform(s, toFar, true).Shape();
        TopoDS_Compound kept;
        BRep_Builder b;
        b.MakeCompound(kept);
        double keptVolume = 0.0;
        int count = 0;
        for (TopExp_Explorer e(splitter.Shape(), TopAbs_SOLID); e.More(); e.Next()) {
            BRepExtrema_DistShapeShape dist(e.Current(), s);
            if (!dist.IsDone() || dist.Value() > tol)
                continue;
            GProp_GProps props;
            BRepGProp::VolumeProperties(e.Current(), props);
            if (std::abs(props.Mass()) < 1e-9)
                continue;
            BRepExtrema_DistShapeShape far(e.Current(), farCap);
            if (far.IsDone() && far.Value() <= tol)
                throw ModelingError("The object does not cover the whole profile: the extrusion would go past it");
            b.Add(kept, e.Current());
            keptVolume += std::abs(props.Mass());
            ++count;
        }
        if (count == 0 || keptVolume >= std::abs(whole.Mass()) * (1.0 - 1e-6))
            throw ModelingError("The extrusion does not reach the object: check the direction (Flip) or pick "
                                "another face");
        if (count == 1) {
            TopExp_Explorer one(kept, TopAbs_SOLID);
            return wrapShape(one.Current());
        }
        return wrapShape(kept);
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

namespace {

/// A fillet or chamfer result is usable: built, valid and still a solid with volume.
bool usableBlend(const TopoDS_Shape& result)
{
    if (result.IsNull() || !hasSubShapes(result, TopAbs_FACE))
        return false;
    BRepCheck_Analyzer check(result);
    if (!check.IsValid())
        return false;
    GProp_GProps props;
    BRepGProp::VolumeProperties(result, props);
    return std::abs(props.Mass()) > 1e-9;
}

/// "2.4 mm" style: two significant digits, rounded down so the size really works.
std::string sizeText(double mm)
{
    const double step = mm >= 10.0 ? 1.0 : mm >= 1.0 ? 0.1 : mm >= 0.1 ? 0.01 : 0.001;
    const double down = std::floor(mm / step + 1e-9) * step;
    std::ostringstream os;
    os << std::setprecision(down >= 10.0 ? 4 : 3) << down << " mm";
    return os.str();
}

/// Runs `build(size)` (null shape on failure). When the requested size fails, searches the
/// largest size that works (bisection, a few tries) and throws a message naming it.
TopoDS_Shape blendWithHint(const char* what, const char* sizeName, const char* edgesName, double size,
                           const std::function<TopoDS_Shape(double)>& build)
{
    auto attempt = [&](double s) -> TopoDS_Shape {
        try {
            TopoDS_Shape r = build(s);
            return usableBlend(r) ? r : TopoDS_Shape();
        } catch (const Standard_Failure&) {
            return {};
        } catch (const ModelingError&) {
            return {};
        }
    };
    TopoDS_Shape result = attempt(size);
    if (!result.IsNull())
        return result;
    // Largest working size: 1% of the request first (nothing works at all otherwise), then
    // bisection. Only on failure, and a handful of builds, so a dialog preview stays live.
    double lo = size * 0.01;
    double hi = size;
    if (attempt(lo).IsNull())
        throw ModelingError(std::string(what) + " failed on " + edgesName + " (even a very small " + sizeName +
                            " does not work; try fewer edges or pick them one chain at a time)");
    for (int i = 0; i < 6; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (attempt(mid).IsNull())
            hi = mid;
        else
            lo = mid;
    }
    throw ModelingError(std::string(what) + ": a " + sizeText(size) + " " + sizeName + " does not fit " + edgesName +
                        "; the largest that works is about " + sizeText(lo));
}

std::vector<TopoDS_Edge> pickedEdges(const TopoDS_Shape& s, const std::vector<std::size_t>& edgeIndices,
                                     const char* what)
{
    const auto map = mapOf(s, TopAbs_EDGE);
    std::vector<TopoDS_Edge> out;
    for (const std::size_t i : edgeIndices) {
        if (i >= static_cast<std::size_t>(map.Extent()))
            throw ModelingError(std::string(what) + ": no edge " + std::to_string(i));
        out.push_back(TopoDS::Edge(map(static_cast<int>(i) + 1)));
    }
    return out;
}

} // namespace

Shape fillet(const Shape& shape, const std::vector<std::size_t>& edgeIndices, double radius)
{
    const TopoDS_Shape& s = occ(shape, "Fillet");
    if (!std::isfinite(radius) || radius <= 0.0)
        throw ModelingError("Fillet: radius must be positive");
    if (edgeIndices.empty())
        throw ModelingError("Fillet: no edges selected");
    const auto picked = pickedEdges(s, edgeIndices, "Fillet");
    const char* edgesName = picked.size() == 1 ? "this edge" : "these edges";
    return wrapShape(blendWithHint("Fillet", "radius", edgesName, radius, [&](double r) {
        BRepFilletAPI_MakeFillet mk(s);
        for (const auto& e : picked)
            mk.Add(r, e);
        mk.Build();
        return mk.IsDone() ? mk.Shape() : TopoDS_Shape();
    }));
}

Shape chamfer(const Shape& shape, const std::vector<std::size_t>& edgeIndices, const ChamferOptions& options)
{
    const TopoDS_Shape& s = occ(shape, "Chamfer");
    auto positive = [](double v) { return std::isfinite(v) && v > 0.0; };
    if (!positive(options.distance))
        throw ModelingError("Chamfer: distance must be positive");
    if (options.type == ChamferType::TwoDistances && !positive(options.distance2))
        throw ModelingError("Chamfer: the second distance must be positive");
    if (options.type == ChamferType::DistanceAngle &&
        !(std::isfinite(options.angle) && options.angle > 0.0 && options.angle < 90.0))
        throw ModelingError("Chamfer: the angle must be between 0 and 90 degrees");
    if (edgeIndices.empty())
        throw ModelingError("Chamfer: no edges selected");
    const auto picked = pickedEdges(s, edgeIndices, "Chamfer");
    // The first face of each edge (the one `distance` is measured on): the first face
    // around it in the body, the other one when flipped.
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    std::vector<TopoDS_Face> firstFaces;
    for (const auto& e : picked) {
        const int i = edgeFaces.FindIndex(e);
        if (i == 0 || edgeFaces(i).Extent() < 2)
            throw ModelingError("Chamfer: the edge is not between two faces");
        firstFaces.push_back(TopoDS::Face(options.flip ? edgeFaces(i).Last() : edgeFaces(i).First()));
    }
    const char* edgesName = picked.size() == 1 ? "this edge" : "these edges";
    // The distances keep their ratio while the largest working size is searched.
    const double ratio = options.distance2 / options.distance;
    return wrapShape(blendWithHint("Chamfer", "distance", edgesName, options.distance, [&](double d) {
        BRepFilletAPI_MakeChamfer mk(s);
        for (std::size_t k = 0; k < picked.size(); ++k) {
            switch (options.type) {
            case ChamferType::EqualDistance:
                mk.Add(d, picked[k]);
                break;
            case ChamferType::TwoDistances:
                mk.Add(d, d * ratio, picked[k], firstFaces[k]);
                break;
            case ChamferType::DistanceAngle:
                mk.AddDA(d, options.angle * std::numbers::pi / 180.0, picked[k], firstFaces[k]);
                break;
            }
        }
        mk.Build();
        return mk.IsDone() ? mk.Shape() : TopoDS_Shape();
    }));
}

Shape chamfer(const Shape& shape, const std::vector<std::size_t>& edgeIndices, double distance)
{
    ChamferOptions options;
    options.distance = distance;
    return chamfer(shape, edgeIndices, options);
}

namespace {

/// Shell fallback: the solid minus its inner offset, opened through the removed faces (flat
/// only) by the prism from each one's inner offset face up to it. A null shape on failure.
TopoDS_Shape shellByInnerOffset(const TopoDS_Shape& s, const TopTools_ListOfShape& removed, double thickness)
{
    try {
        BRepOffsetAPI_MakeOffsetShape inner;
        inner.PerformByJoin(s, -thickness, 1e-3);
        if (!inner.IsDone() || inner.Shape().IsNull())
            return {};
        // A solid offset comes back as a closed shell: make it a solid again.
        TopoDS_Shape tool = inner.Shape();
        if (tool.ShapeType() == TopAbs_SHELL) {
            BRepBuilderAPI_MakeSolid solid(TopoDS::Shell(tool));
            if (!solid.IsDone())
                return {};
            ShapeFix_Solid fix(solid.Solid());
            fix.Perform();
            tool = fix.Solid();
        }
        for (const TopoDS_Shape& removedFace : removed) {
            const TopoDS_Face face = TopoDS::Face(removedFace);
            BRepAdaptor_Surface surface(face);
            if (surface.GetType() != GeomAbs_Plane)
                return {};
            gp_Dir normal = surface.Plane().Axis().Direction();
            if (face.Orientation() == TopAbs_REVERSED)
                normal.Reverse();
            // OCCT lists the offset image of a face as generated from it (copied: the next
            // history call reuses the list).
            TopTools_ListOfShape offsetFaces = inner.Generated(face);
            if (offsetFaces.IsEmpty())
                offsetFaces = inner.Modified(face);
            if (offsetFaces.IsEmpty())
                return {};
            for (const TopoDS_Shape& offsetFace : offsetFaces) {
                // A little past the face, so that no skin is left.
                BRepPrimAPI_MakePrism prism(offsetFace, gp_Vec(normal) * (thickness * 1.05));
                if (!prism.IsDone())
                    return {};
                BRepAlgoAPI_Fuse fuse(tool, prism.Shape());
                if (!fuse.IsDone())
                    return {};
                tool = fuse.Shape();
            }
        }
        BRepAlgoAPI_Cut cut(s, tool);
        if (!cut.IsDone())
            return {};
        ShapeUpgrade_UnifySameDomain unify(cut.Shape());
        unify.Build();
        return unify.Shape();
    } catch (const Standard_Failure&) {
        return {};
    }
}

} // namespace

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
        GProp_GProps before;
        BRepGProp::VolumeProperties(s, before);
        // OCCT can report success with an empty or broken result (seen on filleted boxes):
        // never hand that on as a body.
        auto sound = [&](const TopoDS_Shape& r) {
            if (r.IsNull() || !BRepCheck_Analyzer(r).IsValid())
                return false;
            GProp_GProps after;
            BRepGProp::VolumeProperties(r, after);
            return after.Mass() > 1e-9 && after.Mass() < before.Mass();
        };
        bool built = false;
        try {
            BRepOffsetAPI_MakeThickSolid mk;
            mk.MakeThickSolidByJoin(s, removed, -thickness, 1e-3);
            if (mk.IsDone()) {
                built = true;
                TopoDS_Shape result = mk.Shape();
                if (removed.IsEmpty()) {
                    // With no open face OCCT returns only the inner offset: the hollow part
                    // (Fusion's closed shell) is the solid minus that cavity.
                    BRepAlgoAPI_Cut cut(s, result);
                    result = cut.IsDone() ? cut.Shape() : TopoDS_Shape();
                }
                if (sound(result))
                    return wrapShape(result);
            }
        } catch (const Standard_Failure&) {
        }
        // OCCT cannot open a face that is tangent to a neighbour (the top of a box next to a
        // filleted edge; Fusion shells it). Instead: cut the inner offset of the whole solid
        // out of it, and open each removed flat face by also cutting the prism from its inner
        // offset face up to the face.
        if (const auto result = shellByInnerOffset(s, removed, thickness); sound(result))
            return wrapShape(result);
        if (!built)
            throw ModelingError("Shell failed (thickness too large?)");
        throw ModelingError("Shell failed: walls of this thickness could not be made on this shape "
                            "(try a thinner wall)");
    });
}

Shape pressPull(const Shape& shape, const std::vector<std::size_t>& faceIndices, double distance)
{
    const TopoDS_Shape& s = occ(shape, "Press Pull");
    if (faceIndices.empty())
        throw ModelingError("Press Pull: pick a face");
    if (!std::isfinite(distance) || std::abs(distance) < 1e-6)
        throw ModelingError("Press Pull: distance must not be zero");
    return guarded("Press Pull", [&] {
        const auto map = mapOf(s, TopAbs_FACE);
        for (const std::size_t i : faceIndices)
            if (i >= static_cast<std::size_t>(map.Extent()))
                throw ModelingError("Press Pull: no face " + std::to_string(i));
        // A curved face (Fusion's Offset Face): the picked faces move along their normals
        // by the distance, the other faces stay and are extended or trimmed to meet them.
        bool curved = false;
        for (const std::size_t i : faceIndices)
            curved = curved || !facePlane(shape, i);
        if (curved) {
            // A rounded face that blends smoothly into a face that stays (a fillet) cannot
            // just move: the blend would break. Fusion changes the fillet's radius instead.
            TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
            TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
            for (const std::size_t i : faceIndices) {
                const TopoDS_Face& f = TopoDS::Face(map(static_cast<int>(i) + 1));
                for (TopExp_Explorer e(f, TopAbs_EDGE); e.More(); e.Next()) {
                    const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
                    if (BRep_Tool::Degenerated(edge) || !edgeFaces.Contains(edge))
                        continue;
                    BRepAdaptor_Curve c(edge);
                    const core::Vec3 mid = toVec(c.Value((c.FirstParameter() + c.LastParameter()) / 2));
                    const auto a = faceNormalNear(shape, i, mid);
                    for (const TopoDS_Shape& other : edgeFaces.FindFromKey(edge)) {
                        const auto k = static_cast<std::size_t>(map.FindIndex(other) - 1);
                        if (other.IsSame(f) || std::find(faceIndices.begin(), faceIndices.end(), k) != faceIndices.end())
                            continue;
                        // Tangent: the modeller marked the edge smooth (fillets are), or
                        // the normals agree to within about 0.05 degrees. A shallow sharp
                        // edge (a 2 degree draft) is not a blend.
                        const auto b = faceNormalNear(shape, k, mid);
                        const bool smooth = BRep_Tool::HasContinuity(edge, f, TopoDS::Face(other))
                            && BRep_Tool::Continuity(edge, f, TopoDS::Face(other)) >= GeomAbs_G1;
                        if (smooth || (a && b && a->x * b->x + a->y * b->y + a->z * b->z > 1 - 4e-7))
                            throw ModelingError("Press Pull: face " + std::to_string(i)
                                                + " blends smoothly into the next face (a fillet?); change the fillet's radius instead");
                    }
                }
            }
            BRepOffset_MakeOffset offset;
            offset.Initialize(s, 0.0, 1e-6, BRepOffset_Skin, false, false, GeomAbs_Intersection, false);
            for (const std::size_t i : faceIndices)
                offset.SetOffsetOnFace(TopoDS::Face(map(static_cast<int>(i) + 1)), distance);
            offset.MakeOffsetShape();
            if (!offset.IsDone())
                throw ModelingError("Press Pull failed: the faces could not be offset by " + std::to_string(distance) + " mm");
            TopoDS_Shape result = offset.Shape();
            // The offset can come back as a closed shell rather than a solid (a face with
            // holes in it, for example): close it into a solid.
            if (!result.IsNull() && !hasSubShapes(result, TopAbs_SOLID) && hasSubShapes(result, TopAbs_SHELL)) {
                BRepBuilderAPI_MakeSolid make;
                for (TopExp_Explorer e(result, TopAbs_SHELL); e.More(); e.Next())
                    make.Add(TopoDS::Shell(e.Current()));
                if (make.IsDone()) {
                    ShapeFix_Solid fix;
                    fix.Init(make.Solid());
                    fix.Perform();
                    result = fix.Solid();
                }
            }
            if (result.IsNull() || !hasSubShapes(result, TopAbs_SOLID))
                throw ModelingError("Press Pull: nothing left of the body (distance too large?)");
            result = unify(result);
            if (!BRepCheck_Analyzer(result).IsValid())
                throw ModelingError("Press Pull failed: the result is not a valid solid");
            // A push deeper than a face's curvature turns the face inside out (a 5 mm
            // cylinder pushed 6 mm becomes a 1 mm one facing the other way): a point of each
            // picked face, moved by the distance, must sit on a face that still faces the same
            // way.
            const Shape out = wrapShape(result);
            for (const std::size_t i : faceIndices) {
                const TopoDS_Face& f = TopoDS::Face(map(static_cast<int>(i) + 1));
                // A point on the face itself: the middle of its parameter range can lie in a
                // hole or a cut-away part, so take the grid point nearest the middle that
                // the face's boundary keeps.
                BRepAdaptor_Surface surf(f);
                const double u0 = surf.FirstUParameter(), u1 = surf.LastUParameter();
                const double v0 = surf.FirstVParameter(), v1 = surf.LastVParameter();
                std::optional<gp_Pnt2d> uv;
                double best = 0;
                constexpr int grid = 17; // odd: the middle itself is tried
                for (int a = 0; a < grid; ++a)
                    for (int b = 0; b < grid; ++b) {
                        const double ta = (a + 0.5) / grid, tb = (b + 0.5) / grid;
                        const gp_Pnt2d c(u0 + (u1 - u0) * ta, v0 + (v1 - v0) * tb);
                        const double d = (ta - 0.5) * (ta - 0.5) + (tb - 0.5) * (tb - 0.5);
                        if (uv && d >= best)
                            continue;
                        if (BRepClass_FaceClassifier(f, c, 1e-7).State() == TopAbs_IN) {
                            uv = c;
                            best = d;
                        }
                    }
                if (!uv)
                    continue;
                BRepLProp_SLProps lp(surf, uv->X(), uv->Y(), 1, 1e-6);
                if (!lp.IsNormalDefined())
                    continue;
                gp_Dir n = lp.Normal();
                if (f.Orientation() == TopAbs_REVERSED)
                    n.Reverse();
                const gp_Pnt p = lp.Value().Translated(gp_Vec(n) * distance);
                const core::Vec3 q = toVec(p);
                const auto j = nearestFace(out, q);
                const auto m = j ? faceNormalNear(out, *j, q) : std::nullopt;
                if (!m || m->x * n.X() + m->y * n.Y() + m->z * n.Z() < 0.5)
                    throw ModelingError("Press Pull: the distance is larger than the face's curve allows; try a smaller one");
            }
            return out;
        }
        // Every slab is made from the faces of the original shape, then all are joined to
        // it (pull) or cut from it (push) at once.
        TopTools_ListOfShape slabs;
        for (const std::size_t i : faceIndices) {
            const auto plane = facePlane(shape, i);
            const core::Vec3 v = plane->normal * distance;
            BRepPrimAPI_MakePrism prism(map(static_cast<int>(i) + 1), gp_Vec(v.x, v.y, v.z));
            if (!prism.IsDone())
                throw ModelingError("Press Pull failed on face " + std::to_string(i));
            slabs.Append(prism.Shape());
        }
        TopTools_ListOfShape args;
        args.Append(s);
        TopoDS_Shape result;
        auto run = [&](auto&& algo) {
            algo.SetArguments(args);
            algo.SetTools(slabs);
            algo.SetRunParallel(false);
            algo.Build();
            if (!algo.IsDone() || algo.HasErrors())
                throw ModelingError("Press Pull failed");
            result = algo.Shape();
        };
        if (distance > 0)
            run(BRepAlgoAPI_Fuse());
        else
            run(BRepAlgoAPI_Cut());
        if (result.IsNull() || !hasSubShapes(result, TopAbs_SOLID))
            throw ModelingError("Press Pull: nothing left of the body (distance too large?)");
        result = unify(result);
        if (!BRepCheck_Analyzer(result).IsValid())
            throw ModelingError("Press Pull failed: the result is not a valid solid");
        return wrapShape(result);
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

std::optional<core::Vec3> faceNormalNear(const Shape& shape, std::size_t faceIndex, const core::Vec3& point)
{
    if (shape.isNull())
        return std::nullopt;
    return guarded("Face normal", [&]() -> std::optional<core::Vec3> {
        const auto map = mapOf(shape.impl()->shape, TopAbs_FACE);
        if (faceIndex >= static_cast<std::size_t>(map.Extent()))
            return std::nullopt;
        const TopoDS_Face& f = TopoDS::Face(map(static_cast<int>(faceIndex) + 1));
        BRepExtrema_DistShapeShape dist(BRepBuilderAPI_MakeVertex(toPnt(point)).Vertex(), f);
        if (!dist.IsDone() || dist.NbSolution() < 1)
            return std::nullopt;
        double u = 0, v = 0;
        BRepAdaptor_Surface surf(f);
        if (dist.SupportTypeShape2(1) == BRepExtrema_IsInFace) {
            dist.ParOnFaceS2(1, u, v);
        } else {
            GeomAPI_ProjectPointOnSurf proj(dist.PointOnShape2(1), BRep_Tool::Surface(f));
            if (proj.NbPoints() < 1)
                return std::nullopt;
            proj.LowerDistanceParameters(u, v);
        }
        BRepLProp_SLProps lp(surf, u, v, 1, 1e-6);
        if (!lp.IsNormalDefined())
            return std::nullopt;
        gp_Dir n = lp.Normal();
        if (f.Orientation() == TopAbs_REVERSED)
            n.Reverse();
        return core::Vec3{n.X(), n.Y(), n.Z()};
    });
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
