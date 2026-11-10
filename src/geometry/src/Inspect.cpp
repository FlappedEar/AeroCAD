// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Inspect.hpp"

#include "aerocad/core/MeshSection.hpp"
#include "aerocad/core/Triangulate.hpp"

#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/OcctShape.hpp"
#include "aerocad/geometry/Sketch.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Builder.hxx>
#include <Geom_BSplineCurve.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopoDS_Compound.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <gp_Lin.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Surface.hxx>
#include <ShapeAnalysis_Surface.hxx>
#include <ShapeFix_Face.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
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
        auto axis = [](const gp_Dir& d) { return Direction{{d.X(), d.Y(), d.Z()}, false, true}; };
        if (sub.kind == SubShape::Kind::Face) {
            const TopoDS_Face f = TopoDS::Face(resolve(shape, sub));
            BRepAdaptor_Surface surf(f);
            switch (surf.GetType()) {
            case GeomAbs_Plane: {
                gp_Dir n = surf.Plane().Axis().Direction();
                if (f.Orientation() == TopAbs_REVERSED)
                    n.Reverse();
                return Direction{{n.X(), n.Y(), n.Z()}, true};
            }
            // Round faces measure by their axis, like Fusion (a hole against a face).
            case GeomAbs_Cylinder: return axis(surf.Cylinder().Axis().Direction());
            case GeomAbs_Cone: return axis(surf.Cone().Axis().Direction());
            default: return std::nullopt;
            }
        }
        if (sub.kind == SubShape::Kind::Edge) {
            const TopoDS_Edge e = TopoDS::Edge(resolve(shape, sub));
            BRepAdaptor_Curve c(e);
            if (c.GetType() == GeomAbs_Line) {
                const gp_Dir d = c.Line().Direction();
                return Direction{{d.X(), d.Y(), d.Z()}, false};
            }
            if (c.GetType() == GeomAbs_Circle)
                return axis(c.Circle().Axis().Direction());
            return std::nullopt;
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

std::optional<ShapeDistance> wallThickness(const Shape& shape, const SubShape& face, const core::Vec3& point)
{
    if (face.kind != SubShape::Kind::Face || shape.isNull() || shape.solidCount() == 0)
        return std::nullopt;
    return guarded("Thickness", [&]() -> std::optional<ShapeDistance> {
        const TopoDS_Face f = TopoDS::Face(resolve(shape, face));
        const Handle(Geom_Surface) surface = BRep_Tool::Surface(f);
        if (surface.IsNull())
            return std::nullopt;
        // The click is on the display mesh, a little off the exact face: project it first.
        GeomAPI_ProjectPointOnSurf proj(toPnt(point), surface);
        if (proj.NbPoints() < 1)
            return std::nullopt;
        double u = 0.0;
        double v = 0.0;
        proj.LowerDistanceParameters(u, v);
        BRepLProp_SLProps props(BRepAdaptor_Surface(f), u, v, 1, 1e-9);
        if (!props.IsNormalDefined())
            return std::nullopt;
        gp_Dir n = props.Normal();
        if (f.Orientation() == TopAbs_REVERSED)
            n.Reverse();
        const gp_Pnt start = props.Value();
        // Straight inwards (against the outward face normal) to the first other wall.
        const double size = shape.bounds().diagonal();
        const double eps = std::max(size * 1e-7, 1e-7);
        IntCurvesFace_ShapeIntersector hits;
        hits.Load(shape.impl()->shape, 1e-7);
        hits.Perform(gp_Lin(start, n.Reversed()), eps, 2.0 * size + 1.0);
        if (!hits.IsDone() || hits.NbPnt() < 1)
            return std::nullopt;
        int nearest = 0;
        for (int i = 1; i <= hits.NbPnt(); ++i) {
            if (hits.WParameter(i) <= eps * 10.0)
                continue; // the start face itself
            if (nearest == 0 || hits.WParameter(i) < hits.WParameter(nearest))
                nearest = i;
        }
        if (nearest == 0)
            return std::nullopt;
        return ShapeDistance{hits.WParameter(nearest), toVec(start), toVec(hits.Pnt(nearest))};
    });
}

core::TriangleMesh subShapeMesh(const Shape& shape, const SubShape& sub)
{
    if (sub.kind != SubShape::Kind::Face && sub.kind != SubShape::Kind::Whole)
        return {};
    return guarded("Measure", [&] { return tessellate(wrapShape(resolve(shape, sub))); });
}

std::vector<core::Vec3> subShapePoints(const Shape& shape, const SubShape& sub, double maxStep)
{
    return guarded("Measure", [&] {
        std::vector<core::Vec3> out;
        const TopoDS_Shape s = resolve(shape, sub);
        if (sub.kind == SubShape::Kind::Vertex) {
            out.push_back(toVec(BRep_Tool::Pnt(TopoDS::Vertex(s))));
            return out;
        }
        if (sub.kind != SubShape::Kind::Edge) {
            const core::TriangleMesh mesh = tessellate(wrapShape(s));
            out.insert(out.end(), mesh.vertices().begin(), mesh.vertices().end());
        }
        // Boundary / edge polylines, subdivided to maxStep.
        const std::vector<core::Vec3> segs = edgeSegments(wrapShape(s));
        const double step = std::max(maxStep, 1e-6);
        for (std::size_t i = 0; i + 1 < segs.size(); i += 2) {
            const core::Vec3 a = segs[i];
            const core::Vec3 d = segs[i + 1] - a;
            const auto n = static_cast<std::size_t>(std::min(1e5, std::ceil(d.length() / step)));
            for (std::size_t k = 0; k <= std::max<std::size_t>(n, 1); ++k)
                out.push_back(a + d * (static_cast<double>(k) / static_cast<double>(std::max<std::size_t>(n, 1))));
        }
        return out;
    });
}

Shape polylineEdges(const std::vector<core::Vec3>& segments)
{
    return guarded("Measure", [&] {
        std::vector<std::vector<core::Vec3>> chains;
        double size = 0.0;
        for (std::size_t i = 0; i + 1 < segments.size(); i += 2)
            size = std::max(size, (segments[i + 1] - segments[i]).length());
        const double joinTol = std::max(size * 1e-6, 1e-9);
        for (std::size_t i = 0; i + 1 < segments.size(); i += 2) {
            const core::Vec3& a = segments[i];
            const core::Vec3& b = segments[i + 1];
            if ((b - a).length() <= joinTol)
                continue;
            bool joined = false;
            if (!chains.empty() && (chains.back().back() - a).length() <= joinTol) {
                auto& c = chains.back();
                const core::Vec3 prev = (c.back() - c[c.size() - 2]).normalized();
                const core::Vec3 next = (b - a).normalized();
                if (prev.dot(next) >= std::cos(30.0 * std::numbers::pi / 180.0)) {
                    c.push_back(b);
                    joined = true;
                } else {
                    chains.push_back({a, b}); // a corner: a new edge
                    joined = true;
                }
            }
            if (!joined)
                chains.push_back({a, b});
        }
        if (chains.empty())
            throw ModelingError("Measure: no lines");
        BRep_Builder builder;
        TopoDS_Compound compound;
        builder.MakeCompound(compound);
        for (const auto& c : chains) {
            if (c.size() == 2) {
                builder.Add(compound, BRepBuilderAPI_MakeEdge(toPnt(c[0]), toPnt(c[1])).Edge());
                continue;
            }
            // One degree-1 B-spline through the points: the drawn polyline as a single edge.
            const int n = static_cast<int>(c.size());
            TColgp_Array1OfPnt poles(1, n);
            TColStd_Array1OfReal knots(1, n);
            TColStd_Array1OfInteger mults(1, n);
            double t = 0.0;
            for (int k = 1; k <= n; ++k) {
                poles(k) = toPnt(c[static_cast<std::size_t>(k - 1)]);
                if (k > 1)
                    t += (c[static_cast<std::size_t>(k - 1)] - c[static_cast<std::size_t>(k - 2)]).length();
                knots(k) = t;
                mults(k) = (k == 1 || k == n) ? 2 : 1;
            }
            Handle(Geom_BSplineCurve) curve = new Geom_BSplineCurve(poles, knots, mults, 1);
            builder.Add(compound, BRepBuilderAPI_MakeEdge(curve).Edge());
        }
        return wrapShape(compound);
    });
}

double interferenceVolume(const Shape& a, const Shape& b)
{
    if (a.isNull() || b.isNull() || a.solidCount() == 0 || b.solidCount() == 0 || !boundsOverlap(a, b))
        return 0.0;
    const Shape common = boolean(a, b, BooleanOp::Intersect);
    return common.isNull() ? 0.0 : common.volume();
}

namespace {

/// Fast cap of one outer loop and its holes: ear clipping on the simplified loops (a few
/// thousand points take milliseconds, the kernel's mesher half a second). Returns false when
/// the result does not cover the expected area, so the caller falls back to the kernel.
bool fastCap(const std::vector<core::Polyline>& loops, std::size_t outer, const std::vector<std::ptrdiff_t>& parent,
             const core::CutPlane& plane, const core::Vec3& n, double tolerance, core::TriangleMesh& out)
{
    core::Vec3 u, v;
    core::planeAxes(n, u, v);
    std::vector<std::vector<core::Point2>> rings;
    double expected = 0.0;
    auto addRing = [&](const core::Polyline& loop, bool isOuter) {
        std::vector<core::Point2> pts;
        for (const auto& p : loop.points) {
            const core::Point2 q{(p - plane.origin).dot(u), (p - plane.origin).dot(v)};
            if (!pts.empty() && std::abs(q[0] - pts.back()[0]) + std::abs(q[1] - pts.back()[1]) < tolerance * 0.1)
                continue;
            pts.push_back(q);
        }
        if (pts.size() > 1 && std::abs(pts.front()[0] - pts.back()[0]) + std::abs(pts.front()[1] - pts.back()[1]) < tolerance * 0.1)
            pts.pop_back();
        pts = core::simplifyRing(pts, tolerance);
        if (pts.size() < 3)
            return false;
        double a = 0.0;
        for (std::size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
            a += pts[j][0] * pts[i][1] - pts[i][0] * pts[j][1];
        expected += isOuter ? std::abs(a) / 2 : -std::abs(a) / 2;
        rings.push_back(std::move(pts));
        return true;
    };
    if (!addRing(loops[outer], true))
        return false;
    for (std::size_t h = 0; h < loops.size(); ++h)
        if (parent[h] == static_cast<std::ptrdiff_t>(outer) && loops[h].closed)
            addRing(loops[h], false); // a degenerate hole is dropped
    if (expected <= 0.0)
        return false;
    const auto idx = core::triangulatePolygon(rings);
    if (idx.size() < 3)
        return false;
    std::vector<core::Point2> flat;
    for (const auto& r : rings)
        flat.insert(flat.end(), r.begin(), r.end());
    auto cross2 = [&](std::uint32_t ia, std::uint32_t ib, std::uint32_t ic) {
        const auto &a = flat[ia], &b = flat[ib], &c = flat[ic];
        return (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
    };
    double got = 0.0;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3)
        got += std::abs(cross2(idx[t], idx[t + 1], idx[t + 2])) / 2;
    if (std::abs(got - expected) > 1e-6 * std::max(expected, 1e-12) + 1e-9)
        return false;
    const auto base = static_cast<std::uint32_t>(out.vertexCount());
    for (const auto& q : flat)
        out.addVertex(plane.origin + u * q[0] + v * q[1]);
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        // u x v == n, so a counter-clockwise triangle already faces along n.
        if (cross2(idx[t], idx[t + 1], idx[t + 2]) >= 0)
            out.addTriangle(base + idx[t], base + idx[t + 1], base + idx[t + 2]);
        else
            out.addTriangle(base + idx[t], base + idx[t + 2], base + idx[t + 1]);
    }
    return true;
}

} // namespace

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
        if (fastCap(loops, i, parent, plane, n, std::max(box.diagonal() * 2e-5, minStep), out))
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

std::size_t refineShapeDistances(const Shape& shape, const std::vector<core::Vec3>& points, std::vector<double>& values,
                                 double threshold, std::size_t maxCount)
{
    if (shape.isNull() || maxCount == 0)
        return 0;
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < values.size() && i < points.size(); ++i)
        if (std::isfinite(values[i]) && std::abs(values[i]) > threshold)
            order.push_back(i);
    if (order.empty())
        return 0;
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return std::abs(values[a]) > std::abs(values[b]); });
    if (order.size() > maxCount)
        order.resize(maxCount);

    // Each face: its surface, its trim classifier, box, and a grid of surface samples
    // that seeds a local projection: a global projection per point (ValueOfUV) took a
    // minute for a deviation map on a scan (CAD-261).
    struct FaceData {
        Handle(ShapeAnalysis_Surface) surface;
        std::unique_ptr<BRepTopAdaptor_FClass2d> classifier;
        Bnd_Box box;
        std::vector<gp_Pnt> samples;
        std::vector<gp_Pnt2d> sampleUV;
    };
    constexpr int seedSpans = 24;
    // Built once per thread: OpenCASCADE's projection and classification objects keep
    // state and are not shared between threads.
    const auto buildFaces = [&shape] {
        std::vector<FaceData> faces;
        for (TopExp_Explorer ex(shape.impl()->shape, TopAbs_FACE); ex.More(); ex.Next()) {
            const TopoDS_Face face = TopoDS::Face(ex.Current());
            const Handle(Geom_Surface) surface = BRep_Tool::Surface(face);
            if (surface.IsNull())
                continue;
            FaceData f;
            f.surface = new ShapeAnalysis_Surface(surface);
            f.classifier = std::make_unique<BRepTopAdaptor_FClass2d>(face, 1e-7);
            // From the geometry, not the triangulation: a safe bound for skipping the face.
            BRepBndLib::Add(face, f.box, false);
            double u0 = 0.0, u1 = 0.0, v0 = 0.0, v1 = 0.0;
            BRepTools::UVBounds(face, u0, u1, v0, v1);
            for (int j = 0; j <= seedSpans; ++j)
                for (int i = 0; i <= seedSpans; ++i) {
                    const gp_Pnt2d uv(u0 + (u1 - u0) * i / seedSpans, v0 + (v1 - v0) * j / seedSpans);
                    f.samples.push_back(surface->Value(uv.X(), uv.Y()));
                    f.sampleUV.push_back(uv);
                }
            faces.push_back(std::move(f));
        }
        return faces;
    };
    std::atomic<std::size_t> replaced{0};
    std::atomic<std::size_t> next{0};
    constexpr std::size_t chunk = 64;
    const auto work = [&] {
        std::vector<FaceData> faces = buildFaces();
        for (std::size_t from = next.fetch_add(chunk); from < order.size(); from = next.fetch_add(chunk))
            for (std::size_t o = from; o < std::min(from + chunk, order.size()); ++o) {
                const std::size_t i = order[o];
                const gp_Pnt p(points[i].x, points[i].y, points[i].z);
                const double current = std::abs(values[i]);
                // The exact distance replaces the mesh one either way: an inscribed triangulation
                // can also be closer than the surface (inside a convex reference).
                // Faces in the order of their nearest sample (likely the closest first). A face
                // whose bounding box is farther than the best distance found is skipped: the box
                // is a safe bound, the samples are not (a bulge between them).
                struct Candidate {
                    double estimate;
                    double bound;
                    std::size_t face;
                    std::size_t seed;
                };
                std::vector<Candidate> candidates;
                for (std::size_t n = 0; n < faces.size(); ++n) {
                    const auto& f = faces[n];
                    Bnd_Box near = f.box;
                    near.Enlarge(2.0 * current + 1e-3);
                    if (near.IsOut(p))
                        continue;
                    std::size_t seed = 0;
                    double seedDistance = std::numeric_limits<double>::infinity();
                    for (std::size_t k = 0; k < f.samples.size(); ++k) {
                        const double d = f.samples[k].SquareDistance(p);
                        if (d < seedDistance) {
                            seedDistance = d;
                            seed = k;
                        }
                    }
                    double xmin = 0.0, ymin = 0.0, zmin = 0.0, xmax = 0.0, ymax = 0.0, zmax = 0.0;
                    f.box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
                    const double dx = std::max({xmin - p.X(), 0.0, p.X() - xmax});
                    const double dy = std::max({ymin - p.Y(), 0.0, p.Y() - ymax});
                    const double dz = std::max({zmin - p.Z(), 0.0, p.Z() - zmax});
                    candidates.push_back({seedDistance, std::sqrt(dx * dx + dy * dy + dz * dz), n, seed});
                }
                std::sort(candidates.begin(), candidates.end(),
                          [](const Candidate& a, const Candidate& b) { return a.estimate < b.estimate; });
                double best = std::numeric_limits<double>::infinity();
                for (const auto& c : candidates) {
                    if (c.bound >= best)
                        continue;
                    auto& f = faces[c.face];
                    try {
                        // Falls back to a global search by itself when the local one fails,
                        // and when it lands farther than the dense-mesh distance allows: a
                        // folded face can have a farther local minimum next to the seed.
                        const double maxGap = current + std::max(0.25 * current, 0.01);
                        const gp_Pnt2d uv = f.surface->NextValueOfUV(f.sampleUV[c.seed], p, 1e-6, maxGap);
                        const double gap = f.surface->Gap();
                        if (gap >= best)
                            continue;
                        if (f.classifier->Perform(uv) == TopAbs_OUT)
                            continue; // projects outside the trimmed face: the neighbour covers it
                        best = gap;
                    } catch (const Standard_Failure&) {
                        // keep the mesh distance
                    }
                }
                if (std::isfinite(best)) {
                    values[i] = values[i] < 0.0 ? -best : best;
                    ++replaced;
                }
            }
    };
    // Each value is written by one thread only.
    const std::size_t threads =
        std::clamp<std::size_t>(order.size() / 500, 1, std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (std::size_t k = 1; k < threads; ++k)
        pool.emplace_back(work);
    work();
    for (auto& t : pool)
        t.join();
    return replaced.load();
}

} // namespace aerocad::geometry
