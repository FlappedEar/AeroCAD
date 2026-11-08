// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Sketch.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_BuilderAlgo.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopoDS_Wire.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
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

Shape curveEdges(const Plane& planeIn, const std::vector<SketchCurve>& curves)
{
    const Plane plane = planeIn.normalized();
    BRep_Builder builder;
    TopoDS_Compound comp;
    builder.MakeCompound(comp);
    if (curves.empty())
        throw ModelingError("No curves given");
    try {
        for (const auto& c : curves)
            builder.Add(comp, makeEdge(plane, c));
    } catch (const Standard_Failure& e) {
        throw ModelingError(std::string("Sketch curve: ") + e.GetMessageString());
    }
    return wrapShape(comp);
}

std::vector<SketchCurve> offsetCurves(const std::vector<SketchCurve>& curves, double distance)
{
    if (curves.empty())
        throw ModelingError("Select the curves to offset");
    if (std::abs(distance) < kTol)
        throw ModelingError("The offset distance must not be zero");
    const Plane xy; // plane coordinates are XY here
    try {
        BRepBuilderAPI_MakeWire wire;
        // Add the edges in an order that keeps the chain connected.
        std::vector<TopoDS_Edge> edges;
        for (const auto& c : curves)
            edges.push_back(makeEdge(xy, c));
        std::vector<bool> used(edges.size(), false);
        wire.Add(edges[0]);
        used[0] = true;
        for (std::size_t added = 1; added < edges.size(); ++added) {
            bool any = false;
            for (std::size_t i = 0; i < edges.size() && !any; ++i) {
                if (used[i])
                    continue;
                wire.Add(edges[i]);
                if (wire.Error() == BRepBuilderAPI_WireDone) {
                    used[i] = true;
                    any = true;
                }
            }
            if (!any)
                throw ModelingError("The selected curves are not one connected chain");
        }
        if (!wire.IsDone())
            throw ModelingError("The selected curves are not one connected chain");
        const TopoDS_Wire w = wire.Wire();
        const bool closed = BRep_Tool::IsClosed(w);
        BRepOffsetAPI_MakeOffset offset;
        offset.Init(GeomAbs_Arc, !closed);
        offset.AddWire(w);
        // OCCT offsets a closed wire outwards for a positive value when it runs
        // counter-clockwise; make "positive = outwards" independent of the drawing order.
        double d = distance;
        if (closed) {
            const TopoDS_Face face = BRepBuilderAPI_MakeFace(w, true).Face();
            GProp_GProps props;
            BRepGProp::SurfaceProperties(face, props);
            BRepGProp_Face f(face);
            double u1, u2, v1, v2;
            f.Bounds(u1, u2, v1, v2);
            gp_Pnt p;
            gp_Vec n;
            f.Normal((u1 + u2) / 2, (v1 + v2) / 2, p, n);
            if (n.Z() < 0)
                d = -d;
        }
        offset.Perform(d);
        if (!offset.IsDone())
            throw ModelingError("The offset could not be made (too large for these curves?)");
        std::vector<SketchCurve> out;
        auto uv = [](const gp_Pnt& p) { return Vec2{p.X(), p.Y()}; };
        // OCCT gives the offset of a spline as several free-form pieces. Consecutive pieces
        // that continue each other smoothly become one spline (separately interpolated
        // pieces would kink where they meet); a closed run of them becomes one closed spline.
        for (TopExp_Explorer we(offset.Shape(), TopAbs_WIRE); we.More(); we.Next()) {
            struct Piece {
                TopoDS_Edge edge;
                bool freeForm;
                double length;
            };
            std::vector<Piece> pieces;
            for (BRepTools_WireExplorer e(TopoDS::Wire(we.Current())); e.More(); e.Next()) {
                const TopoDS_Edge edge = e.Current();
                BRepAdaptor_Curve ac(edge);
                const bool freeForm = ac.GetType() != GeomAbs_Line && ac.GetType() != GeomAbs_Circle;
                pieces.push_back({edge, freeForm, GCPnts_AbscissaPoint::Length(ac)});
            }
            // Point and unit tangent at the start (s = 0) or end (s = 1) of a piece, in the
            // direction the wire runs.
            auto at = [&](const Piece& piece, double s, gp_Vec* tangent) {
                BRepAdaptor_Curve ac(piece.edge);
                const bool reversed = piece.edge.Orientation() == TopAbs_REVERSED;
                const double t0 = ac.FirstParameter(), t1 = ac.LastParameter();
                const double t = reversed ? t1 + (t0 - t1) * s : t0 + (t1 - t0) * s;
                gp_Pnt p;
                gp_Vec d;
                ac.D1(t, p, d);
                if (tangent != nullptr && d.Magnitude() > 1e-12)
                    *tangent = (reversed ? -d : d).Normalized();
                return p;
            };
            auto smooth = [&](const Piece& a, const Piece& b) {
                gp_Vec ta, tb;
                at(a, 1.0, &ta);
                at(b, 0.0, &tb);
                return ta.Angle(tb) < 1e-2;
            };
            const std::size_t n = pieces.size();
            std::vector<bool> taken(n, false);
            // A run of free-form pieces may wrap past the end of a closed wire: start each
            // run at a piece whose predecessor does not continue into it.
            const bool closedWire = BRep_Tool::IsClosed(TopoDS::Wire(we.Current()));
            for (std::size_t i = 0; i < n; ++i) {
                if (taken[i])
                    continue;
                const Piece& piece = pieces[i];
                if (!piece.freeForm) {
                    taken[i] = true;
                    BRepAdaptor_Curve ac(piece.edge);
                    const double t0 = ac.FirstParameter(), t1 = ac.LastParameter();
                    SketchCurve c;
                    if (ac.GetType() == GeomAbs_Line) {
                        c.kind = SketchCurve::Kind::Line;
                        c.points = {uv(ac.Value(t0)), uv(ac.Value(t1))};
                        if (distance2d(c.points[0], c.points[1]) < kTol)
                            continue;
                    } else if (std::abs(t1 - t0 - 2 * M_PI) < 1e-9) {
                        c.kind = SketchCurve::Kind::Circle;
                        c.points = {uv(ac.Circle().Location())};
                        c.radius = ac.Circle().Radius();
                    } else {
                        c.kind = SketchCurve::Kind::Arc;
                        c.points = {uv(ac.Value(t0)), uv(ac.Value((t0 + t1) / 2)), uv(ac.Value(t1))};
                        if (distance2d(c.points[0], c.points[2]) < kTol)
                            continue;
                    }
                    out.push_back(c);
                    continue;
                }
                // Back up to the start of this smooth run (in a closed wire).
                std::size_t first = i;
                if (closedWire) {
                    for (std::size_t k = 0; k < n; ++k) {
                        const std::size_t prev = (first + n - 1) % n;
                        if (prev == i || taken[prev] || !pieces[prev].freeForm || !smooth(pieces[prev], pieces[first]))
                            break;
                        first = prev;
                    }
                }
                std::vector<std::size_t> run{first};
                taken[first] = true;
                for (std::size_t k = 1; k < n; ++k) {
                    const std::size_t next = (first + k) % n;
                    if (!closedWire && next < first)
                        break;
                    if (taken[next] || !pieces[next].freeForm || !smooth(pieces[run.back()], pieces[next]))
                        break;
                    run.push_back(next);
                    taken[next] = true;
                }
                double runLength = 0;
                for (const std::size_t k : run)
                    runLength += pieces[k].length;
                SketchCurve c;
                c.kind = SketchCurve::Kind::Spline;
                for (const std::size_t k : run) {
                    // About 48 points over the run, at least 3 per piece.
                    const int steps = std::max(3, static_cast<int>(std::lround(48 * pieces[k].length / std::max(runLength, 1e-12))));
                    for (int j = 0; j <= steps; ++j) {
                        const Vec2 p = uv(at(pieces[k], static_cast<double>(j) / steps, nullptr));
                        if (c.points.empty() || distance2d(c.points.back(), p) > kTol)
                            c.points.push_back(p);
                    }
                }
                if (c.points.size() < 2)
                    continue;
                // A run that comes back to its start smoothly is one closed (periodic) spline.
                c.closed = run.size() == n && closedWire && smooth(pieces[run.back()], pieces[run.front()])
                           && distance2d(c.points.front(), c.points.back()) < kTol;
                if (c.closed)
                    c.points.pop_back();
                out.push_back(c);
            }
        }
        if (out.empty())
            throw ModelingError("The offset is empty (too large for these curves?)");
        return out;
    } catch (const Standard_Failure& e) {
        throw ModelingError(std::string("Offset failed: ") + e.GetMessageString());
    }
}

std::vector<SketchCurve> projectEdges(const Shape& shape, const std::vector<std::size_t>& edgeIndices,
                                      const Plane& planeIn)
{
    const Plane plane = planeIn.normalized();
    if (shape.isNull())
        throw ModelingError("Nothing to project");
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(shape.impl()->shape, TopAbs_EDGE, map);
    std::vector<SketchCurve> out;
    auto uv = [&](const gp_Pnt& p) { return plane.toLocal({p.X(), p.Y(), p.Z()}); };
    try {
        for (const std::size_t i : edgeIndices) {
            if (i >= static_cast<std::size_t>(map.Extent()))
                throw ModelingError("No edge " + std::to_string(i));
            BRepAdaptor_Curve ac(TopoDS::Edge(map(static_cast<int>(i) + 1)));
            const double t0 = ac.FirstParameter(), t1 = ac.LastParameter();
            SketchCurve c;
            if (ac.GetType() == GeomAbs_Line) {
                c.kind = SketchCurve::Kind::Line;
                c.points = {uv(ac.Value(t0)), uv(ac.Value(t1))};
                if (distance2d(c.points[0], c.points[1]) < kTol)
                    continue; // seen end-on
            } else if (ac.GetType() == GeomAbs_Circle &&
                       std::abs(std::abs(ac.Circle().Axis().Direction().Dot(toDir(plane.normal))) - 1.0) < 1e-9) {
                if (std::abs(t1 - t0 - 2 * M_PI) < 1e-9) {
                    c.kind = SketchCurve::Kind::Circle;
                    c.points = {uv(ac.Circle().Location())};
                    c.radius = ac.Circle().Radius();
                } else {
                    c.kind = SketchCurve::Kind::Arc;
                    c.points = {uv(ac.Value(t0)), uv(ac.Value((t0 + t1) / 2)), uv(ac.Value(t1))};
                }
            } else {
                // A curve in a plane square to the sketch (a circle seen edge-on) projects onto
                // a straight segment: give its extent as a line, not a spline that runs there
                // and back.
                std::vector<Vec2> dense;
                for (int k = 0; k <= 96; ++k)
                    dense.push_back(uv(ac.Value(t0 + (t1 - t0) * k / 96.0)));
                auto farthestFrom = [&](const Vec2& q) {
                    Vec2 best = q;
                    for (const Vec2& p : dense)
                        if (distance2d(p, q) > distance2d(best, q))
                            best = p;
                    return best;
                };
                const Vec2 a = farthestFrom(dense.front());
                const Vec2 b = farthestFrom(a);
                const double len = distance2d(a, b);
                if (len < kTol)
                    continue; // seen end-on: a point
                double off = 0;
                for (const Vec2& p : dense)
                    off = std::max(off, std::abs((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) / len);
                if (off < 1e-6 * std::max(1.0, len)) {
                    c.kind = SketchCurve::Kind::Line;
                    c.points = {a, b};
                    out.push_back(c);
                    continue;
                }
                c.kind = SketchCurve::Kind::Spline;
                for (int k = 0; k <= 24; ++k) {
                    const Vec2 p = uv(ac.Value(t0 + (t1 - t0) * k / 24.0));
                    if (c.points.empty() || distance2d(c.points.back(), p) > kTol)
                        c.points.push_back(p);
                }
                if (c.points.size() < 2)
                    continue;
                c.closed = ac.IsClosed() && distance2d(c.points.front(), c.points.back()) < kTol;
                if (c.closed)
                    c.points.pop_back();
            }
            out.push_back(c);
        }
    } catch (const Standard_Failure& e) {
        throw ModelingError(std::string("Project failed: ") + e.GetMessageString());
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
