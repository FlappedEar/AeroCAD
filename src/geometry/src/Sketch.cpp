// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Sketch.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepAlgoAPI_BuilderAlgo.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GC_MakeSegment.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <set>

namespace aerocad::geometry {

namespace {

constexpr double kTol = 1e-6;

gp_Pnt toPnt(const core::Vec3& v)
{
    return {v.x, v.y, v.z};
}

gp_Dir toDir(const core::Vec3& v)
{
    return {v.x, v.y, v.z};
}

core::Vec3 toVec(const gp_Pnt& p)
{
    return {p.X(), p.Y(), p.Z()};
}

double distance2d(const Vec2& a, const Vec2& b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

TopoDS_Edge makeEdge(const Plane& plane, const SketchCurve& c)
{
    auto requirePoints = [&](std::size_t n, const char* what) {
        if (c.points.size() < n)
            throw ModelingError(std::string(what) + " needs " + std::to_string(n) + " points");
    };
    switch (c.kind) {
    case SketchCurve::Kind::Line: {
        requirePoints(2, "Line");
        if (distance2d(c.points[0], c.points[1]) < kTol)
            throw ModelingError("Line has zero length");
        GC_MakeSegment seg(toPnt(plane.toWorld(c.points[0])), toPnt(plane.toWorld(c.points[1])));
        if (!seg.IsDone())
            throw ModelingError("Invalid line");
        return BRepBuilderAPI_MakeEdge(seg.Value()).Edge();
    }
    case SketchCurve::Kind::Arc: {
        requirePoints(3, "Arc");
        GC_MakeArcOfCircle arc(toPnt(plane.toWorld(c.points[0])), toPnt(plane.toWorld(c.points[1])),
                               toPnt(plane.toWorld(c.points[2])));
        if (!arc.IsDone())
            throw ModelingError("Invalid arc (points are collinear or coincident)");
        return BRepBuilderAPI_MakeEdge(arc.Value()).Edge();
    }
    case SketchCurve::Kind::Circle: {
        requirePoints(1, "Circle");
        if (c.radius <= kTol)
            throw ModelingError("Circle radius must be positive");
        const gp_Ax2 axis(toPnt(plane.toWorld(c.points[0])), toDir(plane.normal), toDir(plane.xDir));
        return BRepBuilderAPI_MakeEdge(gp_Circ(axis, c.radius)).Edge();
    }
    case SketchCurve::Kind::Spline: {
        requirePoints(2, "Spline");
        auto pts = new TColgp_HArray1OfPnt(1, static_cast<int>(c.points.size()));
        for (std::size_t i = 0; i < c.points.size(); ++i)
            pts->SetValue(static_cast<int>(i) + 1, toPnt(plane.toWorld(c.points[i])));
        GeomAPI_Interpolate interp(pts, c.closed, kTol);
        interp.Perform();
        if (!interp.IsDone())
            throw ModelingError("Spline interpolation failed");
        return BRepBuilderAPI_MakeEdge(interp.Curve()).Edge();
    }
    }
    throw ModelingError("Unknown curve kind");
}

/// Removes edges that cannot bound a region (one free end), repeatedly.
std::vector<TopoDS_Edge> pruneDangling(std::vector<TopoDS_Edge> edges)
{
    bool changed = true;
    while (changed) {
        changed = false;
        BRep_Builder builder;
        TopoDS_Compound comp;
        builder.MakeCompound(comp);
        for (const auto& e : edges)
            builder.Add(comp, e);
        TopTools_IndexedDataMapOfShapeListOfShape vertexEdges;
        TopExp::MapShapesAndUniqueAncestors(comp, TopAbs_VERTEX, TopAbs_EDGE, vertexEdges);

        std::vector<TopoDS_Edge> kept;
        for (const auto& e : edges) {
            TopoDS_Vertex v1, v2;
            TopExp::Vertices(e, v1, v2);
            const bool closed = v1.IsSame(v2) || BRep_Tool::IsClosed(e);
            bool dangling = false;
            if (!closed) {
                for (const auto& v : {v1, v2}) {
                    if (v.IsNull() || vertexEdges.FindFromKey(v).Extent() < 2)
                        dangling = true;
                }
            }
            if (dangling)
                changed = true;
            else
                kept.push_back(e);
        }
        edges = std::move(kept);
    }
    return edges;
}

} // namespace

Plane Plane::normalized() const
{
    Plane p;
    p.origin = origin;
    p.normal = normal.normalized();
    if (p.normal.length() < 0.5)
        throw ModelingError("Plane normal must not be zero");
    core::Vec3 x = xDir - p.normal * xDir.dot(p.normal);
    if (x.length() < 1e-9) {
        // Pick the world axis least aligned with the normal.
        const core::Vec3 candidates[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        double best = 2.0;
        for (const auto& c : candidates) {
            const double d = std::abs(c.dot(p.normal));
            if (d < best) {
                best = d;
                x = c - p.normal * c.dot(p.normal);
            }
        }
    }
    p.xDir = x.normalized();
    return p;
}

SketchGeometry buildSketch(const Plane& planeIn, const std::vector<SketchCurve>& curves)
{
    const Plane plane = planeIn.normalized();
    SketchGeometry out;
    try {
        BRep_Builder builder;
        TopoDS_Compound all;
        builder.MakeCompound(all);
        std::vector<TopoDS_Edge> profileEdges;
        for (const auto& c : curves) {
            const TopoDS_Edge e = makeEdge(plane, c);
            builder.Add(all, e);
            if (!c.construction)
                profileEdges.push_back(e);
        }
        out.curves = wrapShape(all);
        if (profileEdges.empty())
            return out;

        // Split all curves at their intersections (general fuse) and merge coincident ends.
        TopTools_ListOfShape args;
        for (const auto& e : profileEdges)
            args.Append(e);
        std::vector<TopoDS_Edge> splitEdges;
        if (args.Extent() == 1) {
            splitEdges = profileEdges;
        } else {
            BRepAlgoAPI_BuilderAlgo fuse;
            fuse.SetArguments(args);
            fuse.SetFuzzyValue(kTol * 10);
            fuse.SetRunParallel(false);
            fuse.Build();
            if (!fuse.IsDone())
                throw ModelingError("Could not intersect sketch curves");
            for (TopExp_Explorer ex(fuse.Shape(), TopAbs_EDGE); ex.More(); ex.Next())
                splitEdges.push_back(TopoDS::Edge(ex.Current()));
        }
        splitEdges = pruneDangling(std::move(splitEdges));
        if (splitEdges.empty())
            return out;

        // A frame face around everything, split by the edges: every split face that does
        // not touch the frame corners is a closed region.
        core::BoundingBox local;
        for (const auto& e : splitEdges) {
            for (TopExp_Explorer ex(e, TopAbs_VERTEX); ex.More(); ex.Next()) {
                const Vec2 p = plane.toLocal(toVec(BRep_Tool::Pnt(TopoDS::Vertex(ex.Current()))));
                local.extend({p.x, p.y, 0});
            }
            // Sample curves too (circles have a single vertex).
            double f = 0, l = 0;
            const auto curve = BRep_Tool::Curve(e, f, l);
            if (!curve.IsNull()) {
                for (int i = 0; i <= 16; ++i) {
                    const Vec2 p = plane.toLocal(toVec(curve->Value(f + (l - f) * i / 16.0)));
                    local.extend({p.x, p.y, 0});
                }
            }
        }
        const double margin = std::max(local.diagonal(), 1.0);
        const double u0 = local.min.x - margin, u1 = local.max.x + margin;
        const double v0 = local.min.y - margin, v1 = local.max.y + margin;
        const gp_Pln pln(gp_Ax3(toPnt(plane.origin), toDir(plane.normal), toDir(plane.xDir)));
        const TopoDS_Face frame = BRepBuilderAPI_MakeFace(pln, u0, u1, v0, v1).Face();

        TopTools_ListOfShape objects, tools;
        objects.Append(frame);
        for (const auto& e : splitEdges)
            tools.Append(e);
        BRepAlgoAPI_Splitter splitter;
        splitter.SetArguments(objects);
        splitter.SetTools(tools);
        splitter.SetFuzzyValue(kTol * 10);
        splitter.SetRunParallel(false);
        splitter.Build();
        if (!splitter.IsDone())
            throw ModelingError("Could not build sketch profiles");

        const double cornerTol = margin * 1e-6 + kTol;
        auto touchesFrame = [&](const TopoDS_Face& f) {
            for (TopExp_Explorer ex(f, TopAbs_VERTEX); ex.More(); ex.Next()) {
                const Vec2 p = plane.toLocal(toVec(BRep_Tool::Pnt(TopoDS::Vertex(ex.Current()))));
                const bool onU = std::abs(p.x - u0) < cornerTol || std::abs(p.x - u1) < cornerTol;
                const bool onV = std::abs(p.y - v0) < cornerTol || std::abs(p.y - v1) < cornerTol;
                if (onU || onV)
                    return true;
            }
            return false;
        };

        struct Region {
            Shape face;
            Vec2 c;
        };
        std::vector<Region> regions;
        for (TopExp_Explorer ex(splitter.Shape(), TopAbs_FACE); ex.More(); ex.Next()) {
            TopoDS_Face f = TopoDS::Face(ex.Current());
            if (touchesFrame(f))
                continue;
            // Orient the profile so its normal matches the sketch normal.
            if (f.Orientation() == TopAbs_REVERSED)
                f.Orientation(TopAbs_FORWARD);
            GProp_GProps props;
            BRepGProp::SurfaceProperties(f, props);
            if (props.Mass() < kTol)
                continue;
            regions.push_back({wrapShape(f), plane.toLocal(toVec(props.CentreOfMass()))});
        }
        std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) {
            if (std::abs(a.c.x - b.c.x) > 1e-6)
                return a.c.x < b.c.x;
            return a.c.y < b.c.y;
        });
        for (auto& r : regions)
            out.profiles.push_back(std::move(r.face));
    } catch (const Standard_Failure& e) {
        throw ModelingError(std::string("Sketch failed: ") + e.GetMessageString());
    }
    return out;
}

double area(const Shape& shape)
{
    if (shape.isNull())
        return 0.0;
    GProp_GProps props;
    BRepGProp::SurfaceProperties(shape.impl()->shape, props);
    return props.Mass();
}

core::Vec3 centroid(const Shape& shape)
{
    if (shape.isNull())
        return {};
    GProp_GProps props;
    BRepGProp::SurfaceProperties(shape.impl()->shape, props);
    return toVec(props.CentreOfMass());
}

bool faceContains(const Shape& face, const core::Vec3& point)
{
    if (face.isNull())
        return false;
    for (TopExp_Explorer ex(face.impl()->shape, TopAbs_FACE); ex.More(); ex.Next()) {
        BRepClass_FaceClassifier classifier(TopoDS::Face(ex.Current()), toPnt(point), 1e-4);
        if (classifier.State() == TopAbs_IN || classifier.State() == TopAbs_ON)
            return true;
    }
    return false;
}

} // namespace aerocad::geometry
