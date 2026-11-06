// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Inspect.hpp"

#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/OcctShape.hpp"
#include "aerocad/geometry/Sketch.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Face.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

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

template <typename Fn>
auto guarded(const char* what, Fn&& fn)
{
    try {
        return fn();
    } catch (const Standard_Failure& e) {
        throw ModelingError(std::string(what) + " failed: " + e.GetMessageString());
    }
}

TopTools_IndexedMapOfShape mapOf(const TopoDS_Shape& s, TopAbs_ShapeEnum type)
{
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(s, type, map);
    return map;
}

TopAbs_ShapeEnum topType(SubShape::Kind kind)
{
    switch (kind) {
    case SubShape::Kind::Face: return TopAbs_FACE;
    case SubShape::Kind::Edge: return TopAbs_EDGE;
    case SubShape::Kind::Vertex: return TopAbs_VERTEX;
    case SubShape::Kind::Whole: break;
    }
    return TopAbs_SHAPE;
}

TopoDS_Shape resolve(const Shape& shape, const SubShape& sub)
{
    if (shape.isNull())
        throw ModelingError("Measure: empty shape");
    const TopoDS_Shape& s = shape.impl()->shape;
    if (sub.kind == SubShape::Kind::Whole)
        return s;
    const auto map = mapOf(s, topType(sub.kind));
    if (sub.index >= static_cast<std::size_t>(map.Extent()))
        throw ModelingError(std::string("Measure: no ") + subShapeKindName(sub.kind) + " with index " +
                            std::to_string(sub.index));
    return map(static_cast<int>(sub.index) + 1);
}

ShapeDistance minDistance(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    BRepExtrema_DistShapeShape dist(a, b);
    if (!dist.IsDone() || dist.NbSolution() < 1)
        throw ModelingError("Measure: distance computation failed");
    return {dist.Value(), toVec(dist.PointOnShape1(1)), toVec(dist.PointOnShape2(1))};
}

} // namespace

const char* subShapeKindName(SubShape::Kind kind)
{
    switch (kind) {
    case SubShape::Kind::Face: return "face";
    case SubShape::Kind::Edge: return "edge";
    case SubShape::Kind::Vertex: return "vertex";
    case SubShape::Kind::Whole: break;
    }
    return "body";
}

std::vector<VertexInfo> vertices(const Shape& shape)
{
    std::vector<VertexInfo> out;
    if (shape.isNull())
        return out;
    const auto map = mapOf(shape.impl()->shape, TopAbs_VERTEX);
    for (int i = 1; i <= map.Extent(); ++i)
        out.push_back({static_cast<std::size_t>(i - 1), toVec(BRep_Tool::Pnt(TopoDS::Vertex(map(i))))});
    return out;
}

std::optional<SubShape> pickSubShape(const Shape& shape, const core::Vec3& point, double tolerance)
{
    if (shape.isNull())
        return std::nullopt;
    return guarded("Pick", [&]() -> std::optional<SubShape> {
        const TopoDS_Shape& s = shape.impl()->shape;
        const gp_Pnt p = toPnt(point);

        const auto verts = mapOf(s, TopAbs_VERTEX);
        std::optional<SubShape> best;
        double bestDist = tolerance;
        for (int i = 1; i <= verts.Extent(); ++i) {
            const double d = BRep_Tool::Pnt(TopoDS::Vertex(verts(i))).Distance(p);
            if (d <= bestDist) {
                bestDist = d;
                best = SubShape{SubShape::Kind::Vertex, static_cast<std::size_t>(i - 1)};
            }
        }
        if (best)
            return best;

        const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(p).Vertex();
        auto nearestOf = [&](TopAbs_ShapeEnum type, double limit) -> std::optional<std::pair<std::size_t, double>> {
            const auto map = mapOf(s, type);
            std::optional<std::pair<std::size_t, double>> found;
            for (int i = 1; i <= map.Extent(); ++i) {
                if (type == TopAbs_EDGE && BRep_Tool::Degenerated(TopoDS::Edge(map(i))))
                    continue;
                BRepExtrema_DistShapeShape dist(v, map(i));
                if (!dist.IsDone())
                    continue;
                if (dist.Value() <= limit && (!found || dist.Value() < found->second - 1e-12))
                    found = std::make_pair(static_cast<std::size_t>(i - 1), dist.Value());
            }
            return found;
        };
        if (const auto e = nearestOf(TopAbs_EDGE, tolerance))
            return SubShape{SubShape::Kind::Edge, e->first};
        if (const auto f = nearestOf(TopAbs_FACE, std::numeric_limits<double>::infinity()))
            return SubShape{SubShape::Kind::Face, f->first};
        return SubShape{};
    });
}

ShapeDistance distance(const Shape& a, const SubShape& subA, const Shape& b, const SubShape& subB)
{
    return guarded("Measure", [&] { return minDistance(resolve(a, subA), resolve(b, subB)); });
}

ShapeDistance distance(const core::Vec3& point, const Shape& b, const SubShape& subB)
{
    return guarded("Measure", [&] {
        const TopoDS_Vertex v = BRepBuilderAPI_MakeVertex(toPnt(point)).Vertex();
        return minDistance(v, resolve(b, subB));
    });
}

std::optional<Direction> subShapeDirection(const Shape& shape, const SubShape& sub)
{
    return guarded("Measure", [&]() -> std::optional<Direction> {
        if (sub.kind == SubShape::Kind::Face) {
            const TopoDS_Face f = TopoDS::Face(resolve(shape, sub));
            BRepAdaptor_Surface surf(f);
            if (surf.GetType() != GeomAbs_Plane)
                return std::nullopt;
            gp_Dir n = surf.Plane().Axis().Direction();
            if (f.Orientation() == TopAbs_REVERSED)
                n.Reverse();
            return Direction{{n.X(), n.Y(), n.Z()}, true};
        }
        if (sub.kind == SubShape::Kind::Edge) {
            const TopoDS_Edge e = TopoDS::Edge(resolve(shape, sub));
            BRepAdaptor_Curve c(e);
            if (c.GetType() != GeomAbs_Line)
                return std::nullopt;
            const gp_Dir d = c.Line().Direction();
            return Direction{{d.X(), d.Y(), d.Z()}, false};
        }
        return std::nullopt;
    });
}

SubShapeProperties subShapeProperties(const Shape& shape, const SubShape& sub)
{
    return guarded("Measure", [&] {
        SubShapeProperties props;
        const TopoDS_Shape s = resolve(shape, sub);
        switch (sub.kind) {
        case SubShape::Kind::Vertex:
            props.type = "vertex";
            props.point = toVec(BRep_Tool::Pnt(TopoDS::Vertex(s)));
            break;
        case SubShape::Kind::Edge: {
            const auto all = edges(shape);
            if (sub.index < all.size()) {
                props.type = all[sub.index].type;
                props.length = all[sub.index].length;
                props.point = all[sub.index].middle;
            }
            BRepAdaptor_Curve c(TopoDS::Edge(s));
            if (c.GetType() == GeomAbs_Circle)
                props.radius = c.Circle().Radius();
            break;
        }
        case SubShape::Kind::Face: {
            const auto all = faces(shape);
            if (sub.index < all.size()) {
                props.type = all[sub.index].type;
                props.area = all[sub.index].area;
                props.point = all[sub.index].center;
            }
            BRepAdaptor_Surface surf(TopoDS::Face(s));
            switch (surf.GetType()) {
            case GeomAbs_Cylinder: props.radius = surf.Cylinder().Radius(); break;
            case GeomAbs_Sphere: props.radius = surf.Sphere().Radius(); break;
            case GeomAbs_Cone: props.radius = surf.Cone().RefRadius(); break;
            default: break;
            }
            break;
        }
        case SubShape::Kind::Whole: {
            props.type = shape.solidCount() > 0 ? "solid" : "shape";
            GProp_GProps surface;
            BRepGProp::SurfaceProperties(s, surface);
            props.area = surface.Mass();
            props.volume = shape.volume();
            if (props.volume > 0.0) {
                GProp_GProps volume;
                BRepGProp::VolumeProperties(s, volume);
                props.point = toVec(volume.CentreOfMass());
            } else {
                props.point = toVec(surface.CentreOfMass());
            }
            break;
        }
        }
        return props;
    });
}

double interferenceVolume(const Shape& a, const Shape& b)
{
    if (a.isNull() || b.isNull() || a.solidCount() == 0 || b.solidCount() == 0 || !boundsOverlap(a, b))
        return 0.0;
    const Shape common = boolean(a, b, BooleanOp::Intersect);
    return common.isNull() ? 0.0 : common.volume();
}

core::TriangleMesh triangulateRegion(const std::vector<core::Polyline>& loops, const core::CutPlane& plane)
{
    core::TriangleMesh out;
    const core::Vec3 n = plane.normal.normalized();
    if (n.length() == 0.0)
        return out;
    const auto parent = core::nestingParents(loops, plane);
    const auto depth = core::nestingDepths(loops, plane);

    core::BoundingBox box;
    for (const auto& l : loops)
        for (const auto& p : l.points)
            box.extend(p);
    // Merge points closer than this (scan sections have many tiny segments).
    const double minStep = std::max(box.diagonal() * 1e-6, 1e-7);

    auto makeWire = [&](const core::Polyline& loop) -> std::optional<TopoDS_Wire> {
        BRepBuilderAPI_MakePolygon poly;
        core::Vec3 last = loop.points.front();
        poly.Add(toPnt(last));
        std::size_t added = 1;
        for (std::size_t i = 1; i < loop.points.size(); ++i) {
            if ((loop.points[i] - last).length() < minStep)
                continue;
            last = loop.points[i];
            poly.Add(toPnt(last));
            ++added;
        }
        if (added < 3)
            return std::nullopt;
        poly.Close();
        if (!poly.IsDone())
            return std::nullopt;
        return poly.Wire();
    };

    const gp_Pln pln(toPnt(plane.origin), gp_Dir(n.x, n.y, n.z));
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (!loops[i].closed || loops[i].points.size() < 3 || depth[i] % 2 != 0)
            continue;
        try {
            const auto outer = makeWire(loops[i]);
            if (!outer)
                continue;
            BRepBuilderAPI_MakeFace maker(pln, *outer, true);
            if (!maker.IsDone())
                continue;
            for (std::size_t h = 0; h < loops.size(); ++h) {
                if (parent[h] != static_cast<std::ptrdiff_t>(i) || !loops[h].closed)
                    continue;
                if (const auto hole = makeWire(loops[h]))
                    maker.Add(*hole);
            }
            ShapeFix_Face fix(maker.Face());
            fix.FixOrientation();
            fix.Perform();
            const core::TriangleMesh mesh = tessellate(wrapShape(fix.Face()));
            // Make every triangle face along the plane normal.
            const auto base = static_cast<std::uint32_t>(out.vertexCount());
            for (const auto& v : mesh.vertices())
                out.addVertex(v);
            for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
                const auto& tri = mesh.triangles()[t];
                if (mesh.faceNormal(t).dot(n) >= 0.0)
                    out.addTriangle(base + tri[0], base + tri[1], base + tri[2]);
                else
                    out.addTriangle(base + tri[0], base + tri[2], base + tri[1]);
            }
        } catch (const Standard_Failure&) {
            continue; // skip loops OCCT cannot turn into a face
        }
    }
    return out;
}

} // namespace aerocad::geometry
