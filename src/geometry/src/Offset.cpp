// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Offset.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <optional>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Surface.hxx>
#include <Precision.hxx>
#include <Bnd_Box.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <BRepGProp.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <BRepTools.hxx>
#include <BRepOffset_MakeSimpleOffset.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace aerocad::geometry {
namespace {

const TopoDS_Shape& occt(const Shape& s)
{
    if (s.isNull())
        throw OffsetError("No surface to offset");
    return s.impl()->shape;
}

void requireSurface(const TopoDS_Shape& s)
{
    if (!TopExp_Explorer(s, TopAbs_FACE).More())
        throw OffsetError("The part has no faces");
    if (TopExp_Explorer(s, TopAbs_SOLID).More())
        throw OffsetError("The part is a solid; offset and thicken work on surfaces");
}

/// BRepOffset_MakeSimpleOffset offsets each face on its own surface (no intersection of
/// neighbours), which is exact and robust for the smooth, single-face base surfaces this
/// is meant for. With the solid flag it also builds the side walls and caps.
TopoDS_Shape simpleOffset(const TopoDS_Shape& s, double distance, bool solid)
{
    BRepOffset_MakeSimpleOffset maker(s, distance);
    maker.SetBuildSolidFlag(solid);
    maker.SetTolerance(1e-4);
    maker.Perform();
    if (!maker.IsDone()) {
        throw OffsetError("Offset failed (" + std::string(maker.GetErrorMessage().ToCString()) +
                          "); the distance may exceed the surface's radius of curvature");
    }
    TopoDS_Shape result = maker.GetResultShape();
    if (!BRepCheck_Analyzer(result).IsValid()) {
        ShapeFix_Shape fix(result);
        fix.Perform();
        result = fix.Shape();
        if (!BRepCheck_Analyzer(result).IsValid())
            throw OffsetError("Offset produced invalid geometry; try a smaller distance");
    }
    return result;
}

/// A single face on its whole underlying surface (natural bounds), without the trim
/// outline and holes: a surface fitted to a brushed selection has a ragged outline, but
/// the B-spline itself covers the selection's bounding rectangle smoothly. Shells of
/// several faces (sewn patches) are returned as they are.
TopoDS_Shape untrimmed(const TopoDS_Shape& s)
{
    TopExp_Explorer e(s, TopAbs_FACE);
    const TopoDS_Face face = TopoDS::Face(e.Current());
    e.Next();
    if (e.More())
        return s;
    const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
    if (surf.IsNull())
        return s;
    BRepBuilderAPI_MakeFace maker(surf, Precision::Confusion());
    if (!maker.IsDone())
        return s;
    TopoDS_Shape full = maker.Face();
    // Keep the face's normal (the side the mesh faced).
    full.Orientation(face.Orientation());
    return full;
}

/// Height of `p` above the surface along `up` (negative: under it), or nothing when it does
/// not project onto the surface.
std::optional<double> heightAbove(const Handle(Geom_Surface)& surf, const gp_Pnt& p, const gp_Vec& up)
{
    GeomAPI_ProjectPointOnSurf projector(p, surf);
    if (projector.NbPoints() == 0)
        return std::nullopt;
    return gp_Vec(projector.NearestPoint(), p).Dot(up);
}

gp_Vec faceNormal(const TopoDS_Face& face)
{
    BRepAdaptor_Surface surface(face);
    gp_Pnt p;
    gp_Vec du, dv;
    surface.D1(0.5 * (surface.FirstUParameter() + surface.LastUParameter()),
               0.5 * (surface.FirstVParameter() + surface.LastVParameter()), p, du, dv);
    gp_Vec n = du.Crossed(dv);
    if (face.Orientation() == TopAbs_REVERSED)
        n.Reverse();
    return n;
}

/// The fin outline with its root carried on down past the surface. A root drawn straight
/// across a curved surface can come out of it towards the ends of the chord (a bumper
/// corner falls away under it), or cross it right at the corners of the outline, where the
/// root's edge would be cut into slivers. The root edges (those whose middle is under the
/// surface) are moved straight down, along -`up` in the outline's plane, and the edges next
/// to them carried on along their own direction: the leading and trailing edges continue
/// cleanly through the surface. The part under the surface is cut off later.
TopoDS_Shape extendRoot(const TopoDS_Shape& outline, const Handle(Geom_Surface)& surf, const gp_Vec& up)
{
    TopTools_IndexedDataMapOfShapeListOfShape vertexEdges;
    TopExp::MapShapesAndAncestors(outline, TopAbs_VERTEX, TopAbs_EDGE, vertexEdges);
    TopTools_MapOfShape rootEdges;
    std::vector<TopoDS_Edge> roots;
    std::vector<double> depths;
    for (TopExp_Explorer e(outline, TopAbs_EDGE); e.More(); e.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(e.Current());
        if (rootEdges.Contains(edge))
            continue;
        BRepAdaptor_Curve curve(edge);
        const double t0 = curve.FirstParameter(), t1 = curve.LastParameter();
        const auto mid = heightAbove(surf, curve.Value(0.5 * (t0 + t1)), up);
        if (!mid || *mid >= 0.0)
            continue;
        const gp_Vec along(curve.Value(t0), curve.Value(t1));
        if (along.Magnitude() < 1e-6 || std::abs(along.Normalized().Dot(up)) > 0.9)
            continue; // a closed or upright edge has no width to carry down
        double high = *mid;
        for (int i = 0; i <= 8; ++i)
            if (const auto h = heightAbove(surf, curve.Value(t0 + (t1 - t0) * i / 8.0), up))
                high = std::max(high, *h);
        rootEdges.Add(edge);
        roots.push_back(edge);
        depths.push_back(std::max(high, 0.0) + 2.0);
    }
    if (roots.empty())
        return outline;

    // Where an end of a root edge goes: on down along the neighbouring edge's own line
    // (when that runs downwards), else straight down; `depth` under the end.
    auto carried = [&](const TopoDS_Edge& root, const TopoDS_Vertex& v, double depth) {
        const gp_Pnt p = BRep_Tool::Pnt(v);
        gp_Vec down = -up;
        const int index = vertexEdges.FindIndex(v);
        if (index > 0) {
            for (TopTools_ListIteratorOfListOfShape it(vertexEdges(index)); it.More(); it.Next()) {
                const TopoDS_Edge next = TopoDS::Edge(it.Value());
                if (next.IsSame(root) || rootEdges.Contains(next))
                    continue;
                BRepAdaptor_Curve curve(next);
                const double t = BRep_Tool::Parameter(v, next);
                gp_Pnt q;
                gp_Vec d;
                curve.D1(t, q, d);
                if (d.Magnitude() < 1e-9)
                    continue;
                // Pointing back out of the neighbouring edge, past the vertex.
                gp_Vec back = d.Normalized();
                if (std::abs(t - curve.FirstParameter()) < std::abs(t - curve.LastParameter()))
                    back.Reverse();
                if (back.Dot(-up) > 0.3)
                    down = back;
            }
        }
        return p.Translated(down * (depth / down.Dot(-up)));
    };

    TopTools_ListOfShape sweeps;
    for (std::size_t k = 0; k < roots.size(); ++k) {
        TopoDS_Vertex a, b;
        TopExp::Vertices(roots[k], a, b);
        const gp_Pnt pa = BRep_Tool::Pnt(a), pb = BRep_Tool::Pnt(b);
        const gp_Pnt qa = carried(roots[k], a, depths[k]), qb = carried(roots[k], b, depths[k]);
        BRepBuilderAPI_MakeWire wire;
        wire.Add(roots[k]);
        if (pb.Distance(qb) > 1e-6)
            wire.Add(BRepBuilderAPI_MakeEdge(pb, qb).Edge());
        wire.Add(BRepBuilderAPI_MakeEdge(qb, qa).Edge());
        if (pa.Distance(qa) > 1e-6)
            wire.Add(BRepBuilderAPI_MakeEdge(qa, pa).Edge());
        if (!wire.IsDone())
            continue;
        BRepBuilderAPI_MakeFace face(wire.Wire(), true);
        if (!face.IsDone())
            continue;
        // Facing the same way as the outline, so the two merge into one face.
        TopoDS_Face f = face.Face();
        if (faceNormal(f).Dot(faceNormal(TopoDS::Face(TopExp_Explorer(outline, TopAbs_FACE).Current()))) < 0.0)
            f.Reverse();
        sweeps.Append(f);
    }
    if (sweeps.IsEmpty())
        return outline;
    TopTools_ListOfShape args;
    args.Append(outline);
    BRepAlgoAPI_Fuse fuse;
    fuse.SetArguments(args);
    fuse.SetTools(sweeps);
    fuse.SetRunParallel(false);
    fuse.Build();
    if (!fuse.IsDone() || fuse.HasErrors())
        return outline;
    // One face, its collinear edges (a leading edge and its continuation) merged.
    ShapeUpgrade_UnifySameDomain unify(fuse.Shape(), true, true, false);
    unify.Build();
    const TopoDS_Shape merged = unify.Shape();
    TopExp_Explorer faces(merged, TopAbs_FACE);
    if (!faces.More())
        return outline;
    const TopoDS_Shape face = faces.Current();
    faces.Next();
    return faces.More() ? outline : face; // could not be merged into one outline: leave it
}

std::string mm(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g", value);
    return buffer;
}

/// All images of `shape` in the result of a boolean: what it became, or itself when untouched.
template <class Algo>
void addImages(Algo& algo, const TopoDS_Shape& shape, TopTools_MapOfShape& into)
{
    if (algo.IsDeleted(shape))
        return;
    const TopTools_ListOfShape& modified = algo.Modified(shape);
    if (modified.IsEmpty()) {
        into.Add(shape);
        return;
    }
    for (TopTools_ListIteratorOfListOfShape it(modified); it.More(); it.Next())
        into.Add(it.Value());
}

/// The fin blanks (outlines extruded, root reaching through `surface`) standing on the
/// `up` side, with a concave fillet of `radius` mm where they meet the surface, and nothing
/// below the surface. A pad under the root (the surface around it, swept down past the
/// root) is fused with the blanks, the edges where the fin walls meet the pad's top (the
/// surface) are filleted, and the pad is cut away again: the fillets flare out exactly
/// onto the surface, tangent to it.
TopoDS_Shape filletedFin(const TopTools_ListOfShape& blanks, const TopoDS_Shape& surface, const gp_Vec& up,
                         const gp_Vec& across, double radius, const Handle(Geom_Surface)& surf)
{
    // Diagnostic checkpoint only: save construction stages without changing inputs.
    static std::atomic<unsigned> probeCounter{0};
    const char* probeDirectory = std::getenv("AEROCAD_CAD95_STAGE_DIR");
    const std::string probePrefix = probeDirectory == nullptr ? std::string() :
        std::string(probeDirectory) + "/fin-" + std::to_string(probeCounter.fetch_add(1));
    const auto capture = [&](const char* label, const TopoDS_Shape& shape) {
        if (!probePrefix.empty() && !BRepTools::Write(shape, (probePrefix + "-" + label + ".brep").c_str()))
            throw OffsetError("Could not write CAD-95 diagnostic stage");
    };
    // A frame on the surface under the fin: `a` across its thickness, `b` along its chord.
    gp_Vec a = across - up * across.Dot(up);
    if (a.Magnitude() < 1e-9)
        throw OffsetError("The fin plane lies along the surface");
    a.Normalize();
    const gp_Vec b = up.Crossed(a);
    Bnd_Box box;
    for (TopTools_ListIteratorOfListOfShape it(blanks); it.More(); it.Next())
        BRepBndLib::Add(it.Value(), box);
    double xs[2], ys[2], zs[2];
    box.Get(xs[0], ys[0], zs[0], xs[1], ys[1], zs[1]);
    double a0 = std::numeric_limits<double>::max(), a1 = -a0, b0 = a0, b1 = -a0, u0 = a0, u1 = -a0;
    for (const double x : xs)
        for (const double y : ys)
            for (const double z : zs) {
                const gp_Vec p(x, y, z);
                a0 = std::min(a0, p.Dot(a));
                a1 = std::max(a1, p.Dot(a));
                b0 = std::min(b0, p.Dot(b));
                b1 = std::max(b1, p.Dot(b));
                u0 = std::min(u0, p.Dot(up));
                u1 = std::max(u1, p.Dot(up));
            }
    const double margin = 2.0 * radius + 1.0;
    auto corner = [&](double s, double t) {
        const gp_Vec p = a * s + b * t;
        return gp_Pnt(p.X(), p.Y(), p.Z());
    };
    BRepBuilderAPI_MakePolygon rect(corner(a0 - margin, b0 - margin), corner(a1 + margin, b0 - margin),
                                    corner(a1 + margin, b1 + margin), corner(a0 - margin, b1 + margin), true);
    BRepBuilderAPI_MakeFace rectFace(rect.Wire(), true);
    if (!rectFace.IsDone())
        throw OffsetError("Could not outline the fin root");
    const core::Vec3 upDir{up.X(), up.Y(), up.Z()};
    TopoDS_Shape top;
    try {
        top = occt(trimSurface(wrapShape(surface), {wrapShape(rectFace.Face())}, upDir, true, false));
    } catch (const OffsetError&) {
        throw OffsetError("The surface does not reach around the fin root for a fillet of " + mm(radius) +
                          " mm");
    }
    TopExp_Explorer topFaces(top, TopAbs_FACE);
    const TopoDS_Face topFace = TopoDS::Face(topFaces.Current());
    topFaces.Next();
    if (topFaces.More())
        throw OffsetError("The surface folds under the fin root; the root fillet needs one smooth piece of it");
    Bnd_Box topBox;
    BRepBndLib::Add(topFace, topBox);
    topBox.Get(xs[0], ys[0], zs[0], xs[1], ys[1], zs[1]);
    double topHigh = -std::numeric_limits<double>::max();
    for (const double x : xs)
        for (const double y : ys)
            for (const double z : zs)
                topHigh = std::max(topHigh, gp_Vec(x, y, z).Dot(up));
    // Deep enough that every bit of the blank under the surface is inside the pad.
    const double depth = std::max(topHigh, u1) - u0 + 1.0;
    BRepPrimAPI_MakePrism padMaker(topFace, up * -depth);
    if (!padMaker.IsDone())
        throw OffsetError("Could not build the pad for the root fillet");
    const TopoDS_Shape pad = padMaker.Shape();
    capture("pad", pad);

    TopTools_ListOfShape tools;
    tools.Append(pad);
    BRepAlgoAPI_Fuse fuse;
    fuse.SetArguments(blanks);
    fuse.SetTools(tools);
    fuse.SetRunParallel(false);
    fuse.Build();
    if (!fuse.IsDone() || fuse.HasErrors())
        throw OffsetError("Joining the fin to the surface for the root fillet failed");
    capture("fuse", fuse.Shape());
    TopTools_MapOfShape topImages, finImages;
    addImages(fuse, topFace, topImages);
    for (TopTools_ListIteratorOfListOfShape it(blanks); it.More(); it.Next())
        for (TopExp_Explorer f(it.Value(), TopAbs_FACE); f.More(); f.Next())
            addImages(fuse, f.Current(), finImages);
    TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
    TopExp::MapShapesAndAncestors(fuse.Shape(), TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    BRepFilletAPI_MakeFillet fillet(fuse.Shape());
    int rootEdges = 0;
    for (int i = 1; i <= edgeFaces.Extent(); ++i) {
        bool onTop = false, onFin = false;
        for (TopTools_ListIteratorOfListOfShape it(edgeFaces(i)); it.More(); it.Next()) {
            onTop = onTop || topImages.Contains(it.Value());
            onFin = onFin || finImages.Contains(it.Value());
        }
        if (onTop && onFin) {
            fillet.Add(radius, TopoDS::Edge(edgeFaces.FindKey(i)));
            ++rootEdges;
        }
    }
    if (rootEdges == 0)
        throw OffsetError("The fin outline does not reach through the surface; draw its root a little into the "
                          "surface (past the surface line in the sketch)");
    try {
        fillet.Build();
    } catch (const Standard_Failure&) {
    }
    if (!fillet.IsDone())
        throw OffsetError("A root fillet of " + mm(radius) + " mm does not fit this fin; try a smaller radius");
    capture("fillet", fillet.Shape());
    if (!probePrefix.empty()) {
        // Keep shared topology between the two inputs in the same BREP file.
        TopoDS_Compound inputs;
        BRep_Builder builder;
        builder.MakeCompound(inputs);
        builder.Add(inputs, fillet.Shape());
        builder.Add(inputs, pad);
        builder.Add(inputs, topFace);
        capture("cut-inputs", inputs);
    }

    BRepAlgoAPI_Cut cut(fillet.Shape(), pad);
    if (!cut.IsDone() || cut.HasErrors())
        throw OffsetError("Cutting the fin back to the surface after the root fillet failed");
    capture("cut", cut.Shape());
    std::vector<TopoDS_Shape> kept;
    for (TopExp_Explorer e(cut.Shape(), TopAbs_SOLID); e.More(); e.Next()) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(e.Current(), props);
        if (std::abs(props.Mass()) < 1e-6)
            continue;
        GeomAPI_ProjectPointOnSurf projector(props.CentreOfMass(), surf);
        if (projector.NbPoints() > 0 && gp_Vec(projector.NearestPoint(), props.CentreOfMass()).Dot(up) <= 0.0)
            continue;
        kept.push_back(e.Current());
    }
    if (kept.empty())
        throw OffsetError("The root fillet left nothing of the fin");
    TopoDS_Shape result;
    if (kept.size() == 1) {
        result = kept.front();
    } else {
        TopoDS_Compound c;
        BRep_Builder builder;
        builder.MakeCompound(c);
        for (const auto& k : kept)
            builder.Add(c, k);
        result = c;
    }
    if (!BRepCheck_Analyzer(result).IsValid()) {
        ShapeFix_Shape fix(result);
        fix.Perform();
        result = fix.Shape();
        if (!BRepCheck_Analyzer(result).IsValid())
            throw OffsetError("The root fillet produced invalid geometry; try a smaller radius");
    }
    capture("result", result);
    return result;
}

} // namespace

Shape offsetSurface(const Shape& surface, double distance)
{
    const TopoDS_Shape& s = occt(surface);
    requireSurface(s);
    if (!std::isfinite(distance) || distance == 0.0)
        throw OffsetError("Offset distance must be a non-zero number");
    try {
        return wrapShape(simpleOffset(s, distance, false));
    } catch (const Standard_Failure& e) {
        throw OffsetError(std::string("Offset failed: ") + e.GetMessageString());
    }
}

Shape thickenSurface(const Shape& surface, double thickness, ThickenSide side)
{
    const TopoDS_Shape& s = occt(surface);
    requireSurface(s);
    if (!std::isfinite(thickness) || thickness <= 0.0)
        throw OffsetError("Thickness must be positive");
    try {
        TopoDS_Shape base = s;
        double distance = thickness;
        if (side == ThickenSide::Inside) {
            distance = -thickness;
        } else if (side == ThickenSide::Both) {
            base = simpleOffset(s, -0.5 * thickness, false);
        }
        TopoDS_Shape solid = simpleOffset(base, distance, true);
        if (!TopExp_Explorer(solid, TopAbs_SOLID).More())
            throw OffsetError("Thicken did not produce a closed solid");
        // Thickening along the normal yields a solid whose faces point inwards (negative
        // volume); turn it the right way out.
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid, props);
        if (props.Mass() < 0.0)
            solid.Reverse();
        return wrapShape(solid);
    } catch (const Standard_Failure& e) {
        throw OffsetError(std::string("Thicken failed: ") + e.GetMessageString());
    }
}

Shape trimSurface(const Shape& surface, const std::vector<Shape>& outlines, const core::Vec3& direction,
                  bool keepInside, bool extend)
{
    TopoDS_Shape s = occt(surface);
    requireSurface(s);
    if (extend)
        s = untrimmed(s);
    if (outlines.empty())
        throw OffsetError("No closed outline to cut the surface with");
    const double len = direction.length();
    if (!(len > 0.0))
        throw OffsetError("Projection direction is zero");
    const gp_Vec dir(direction.x / len, direction.y / len, direction.z / len);
    try {
        // Prisms through the outlines, long enough to pass through the whole surface on
        // both sides of the outline plane.
        Bnd_Box box;
        BRepBndLib::Add(s, box);
        for (const auto& o : outlines) {
            if (!o.isNull())
                BRepBndLib::Add(o.impl()->shape, box);
        }
        const double reach = std::sqrt(box.SquareExtent()) + 1.0;
        TopTools_ListOfShape tools;
        for (const auto& o : outlines) {
            if (o.isNull())
                continue;
            gp_Trsf back;
            back.SetTranslation(dir * -reach);
            const TopoDS_Shape start = BRepBuilderAPI_Transform(o.impl()->shape, back, true).Shape();
            BRepPrimAPI_MakePrism prism(start, dir * (2.0 * reach));
            if (!prism.IsDone())
                throw OffsetError("Could not sweep the outline");
            tools.Append(prism.Shape());
        }
        if (tools.IsEmpty())
            throw OffsetError("No closed outline to cut the surface with");

        TopTools_ListOfShape args;
        args.Append(s);
        TopoDS_Shape result;
        auto run = [&](auto&& algo) {
            algo.SetArguments(args);
            algo.SetTools(tools);
            algo.SetRunParallel(false);
            algo.Build();
            if (!algo.IsDone() || algo.HasErrors())
                throw OffsetError("Cutting the surface with the outline failed");
            result = algo.Shape();
        };
        if (keepInside)
            run(BRepAlgoAPI_Common());
        else
            run(BRepAlgoAPI_Cut());

        std::vector<TopoDS_Shape> faces;
        for (TopExp_Explorer e(result, TopAbs_FACE); e.More(); e.Next())
            faces.push_back(e.Current());
        if (faces.empty()) {
            throw OffsetError(keepInside ? "The outline does not overlap the surface (seen along the sketch normal)"
                                         : "The outline covers the whole surface; nothing is left");
        }
        if (faces.size() == 1)
            return wrapShape(faces.front());
        // Several pieces of one surface: sew neighbours into shells so they thicken as one.
        BRepBuilderAPI_Sewing sewing(1e-4);
        for (const auto& f : faces)
            sewing.Add(f);
        sewing.Perform();
        TopoDS_Shape sewn = sewing.SewedShape();
        if (sewn.IsNull()) {
            TopoDS_Compound c;
            BRep_Builder b;
            b.MakeCompound(c);
            for (const auto& f : faces)
                b.Add(c, f);
            sewn = c;
        }
        return wrapShape(sewn);
    } catch (const Standard_Failure& e) {
        throw OffsetError(std::string("Trim failed: ") + e.GetMessageString());
    }
}

Shape finOnSurface(const Shape& surface, const std::vector<Shape>& outlines, const core::Vec3& normal,
                   double thickness, const core::Vec3& outward, double rootFillet)
{
    TopoDS_Shape s = occt(surface);
    requireSurface(s);
    s = untrimmed(s);
    if (outlines.empty())
        throw OffsetError("No closed outline for the fin");
    if (!std::isfinite(thickness) || thickness <= 0.0)
        throw OffsetError("Thickness must be positive");
    if (!std::isfinite(rootFillet) || rootFillet < 0.0)
        throw OffsetError("The root fillet radius must be 0 (none) or positive");
    const double len = normal.length();
    const double outLen = outward.length();
    if (!(len > 0.0) || !(outLen > 0.0))
        throw OffsetError("Fin direction is zero");
    const gp_Vec dir(normal.x / len, normal.y / len, normal.z / len);
    const gp_Vec out(outward.x / outLen, outward.y / outLen, outward.z / outLen);
    try {
        Handle(Geom_Surface) surf;
        for (TopExp_Explorer e(s, TopAbs_FACE); e.More() && surf.IsNull(); e.Next())
            surf = BRep_Tool::Surface(TopoDS::Face(e.Current()));
        // The fin before the cut: the outlines, their roots carried down past the surface,
        // extruded half the thickness each way.
        TopTools_ListOfShape blanks;
        // Which side the fin stands on, told by the outlines as drawn (their area above the
        // surface against the area under it): the extended root must not change it.
        double drawnSide = 0.0;
        auto splitArea = [&](const TopoDS_Shape& outline) {
            TopTools_ListOfShape args, cutters;
            args.Append(outline);
            cutters.Append(s);
            BRepAlgoAPI_Splitter split;
            split.SetArguments(args);
            split.SetTools(cutters);
            split.SetRunParallel(false);
            split.Build();
            double balance = 0.0;
            if (!split.IsDone() || split.HasErrors())
                return balance;
            for (TopExp_Explorer f(split.Shape(), TopAbs_FACE); f.More(); f.Next()) {
                GProp_GProps piece;
                BRepGProp::SurfaceProperties(f.Current(), piece);
                if (const auto h = heightAbove(surf, piece.CentreOfMass(), out))
                    balance += (*h >= 0.0 ? 1.0 : -1.0) * piece.Mass();
            }
            return balance;
        };
        // First the side, from all the outlines together: every root is then carried down
        // away from that one side, so no extension ends up kept as fin material.
        std::vector<TopoDS_Shape> drawn;
        double centroidSide = 0.0;
        for (const auto& o : outlines) {
            if (o.isNull())
                continue;
            drawn.push_back(o.impl()->shape);
            if (surf.IsNull())
                continue;
            GProp_GProps area;
            BRepGProp::SurfaceProperties(drawn.back(), area);
            if (const auto h = heightAbove(surf, area.CentreOfMass(), out)) {
                drawnSide += splitArea(drawn.back());
                centroidSide += *h * area.Mass();
            }
        }
        const bool drawnAbove = drawnSide != 0.0 ? drawnSide > 0.0 : centroidSide >= 0.0;
        for (TopoDS_Shape outline : drawn) {
            if (!surf.IsNull()) {
                GProp_GProps area;
                BRepGProp::SurfaceProperties(outline, area);
                if (heightAbove(surf, area.CentreOfMass(), out))
                    outline = extendRoot(outline, surf, drawnAbove ? out : -out);
            }
            gp_Trsf back;
            back.SetTranslation(dir * (-0.5 * thickness));
            const TopoDS_Shape start = BRepBuilderAPI_Transform(outline, back, true).Shape();
            BRepPrimAPI_MakePrism prism(start, dir * thickness);
            if (!prism.IsDone())
                throw OffsetError("Could not extrude the fin outline");
            blanks.Append(prism.Shape());
        }
        if (blanks.IsEmpty())
            throw OffsetError("No closed outline for the fin");

        TopTools_ListOfShape tools;
        tools.Append(s);
        BRepAlgoAPI_Splitter splitter;
        splitter.SetArguments(blanks);
        splitter.SetTools(tools);
        splitter.SetRunParallel(false);
        splitter.Build();
        if (!splitter.IsDone() || splitter.HasErrors())
            throw OffsetError("Cutting the fin root with the surface failed");

        // Sort the pieces by the side of the surface their centre lies on, and keep the
        // side with more of the fin: the root dipping into the surface is cut off, whichever
        // way the outline was drawn.
        std::vector<TopoDS_Shape> sides[2];
        double volume[2] = {0.0, 0.0};
        for (TopExp_Explorer e(splitter.Shape(), TopAbs_SOLID); e.More(); e.Next()) {
            GProp_GProps props;
            BRepGProp::VolumeProperties(e.Current(), props);
            const gp_Pnt c = props.CentreOfMass();
            int side = 0;
            if (!surf.IsNull()) {
                GeomAPI_ProjectPointOnSurf projector(c, surf);
                if (projector.NbPoints() > 0)
                    side = gp_Vec(projector.NearestPoint(), c).Dot(out) > 0.0 ? 0 : 1;
            }
            sides[side].push_back(e.Current());
            volume[side] += std::abs(props.Mass());
        }
        if (sides[0].empty() || sides[1].empty())
            throw OffsetError("The fin outline does not reach through the surface; draw its root a little into the "
                              "surface (past the surface line in the sketch)");
        const bool alongOut = drawnSide != 0.0 || centroidSide != 0.0 ? drawnAbove : volume[0] >= volume[1];
        const std::vector<TopoDS_Shape>& kept = alongOut ? sides[0] : sides[1];
        TopoDS_Shape result;
        if (rootFillet > 0.0) {
            result = filletedFin(blanks, s, alongOut ? out : -out, dir, rootFillet, surf);
        } else if (kept.size() == 1) {
            result = kept.front();
        } else {
            TopoDS_Compound c;
            BRep_Builder b;
            b.MakeCompound(c);
            for (const auto& k : kept)
                b.Add(c, k);
            result = c;
        }
        GProp_GProps props;
        BRepGProp::VolumeProperties(result, props);
        if (props.Mass() < 0.0)
            result.Reverse();
        return wrapShape(result);
    } catch (const Standard_Failure& e) {
        throw OffsetError(std::string("Fin failed: ") + e.GetMessageString());
    }
}

Shape splitBySurface(const Shape& solid, const Shape& surface, bool keepFront)
{
    if (solid.isNull())
        throw OffsetError("Nothing to cut");
    const TopoDS_Shape& body = occt(solid);
    if (!TopExp_Explorer(body, TopAbs_SOLID).More())
        throw OffsetError("Only solids can be cut by a surface");
    TopoDS_Shape s = occt(surface);
    requireSurface(s);
    s = untrimmed(s);
    try {
        TopTools_ListOfShape args, tools;
        args.Append(body);
        tools.Append(s);
        BRepAlgoAPI_Splitter splitter;
        splitter.SetArguments(args);
        splitter.SetTools(tools);
        splitter.SetRunParallel(false);
        splitter.Build();
        if (!splitter.IsDone() || splitter.HasErrors())
            throw OffsetError("Cutting the body with the surface failed");

        // The cutting surface's oriented normal at the point nearest to `q`.
        auto surfaceNormal = [&](const gp_Pnt& q, gp_Pnt& onSurface) -> std::optional<gp_Vec> {
            double best = std::numeric_limits<double>::max();
            std::optional<gp_Vec> normal;
            for (TopExp_Explorer f(s, TopAbs_FACE); f.More(); f.Next()) {
                const TopoDS_Face face = TopoDS::Face(f.Current());
                const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
                if (surf.IsNull())
                    continue;
                GeomAPI_ProjectPointOnSurf projector(q, surf);
                if (projector.NbPoints() == 0 || projector.LowerDistance() >= best)
                    continue;
                best = projector.LowerDistance();
                double u = 0.0, v = 0.0;
                projector.LowerDistanceParameters(u, v);
                gp_Vec du, dv;
                surf->D1(u, v, onSurface, du, dv);
                gp_Vec n = du.Crossed(dv);
                if (face.Orientation() == TopAbs_REVERSED)
                    n.Reverse();
                normal = n;
            }
            return normal;
        };
        // The faces the surface left in the result: a piece touching it shows which side it
        // is on by its own outward normal there (outward along the surface normal = the
        // piece is behind the surface). Robust for curved surfaces, where a piece's centre
        // of mass can lie on the other side.
        TopTools_MapOfShape cutFaces;
        for (TopExp_Explorer f(s, TopAbs_FACE); f.More(); f.Next()) {
            const TopTools_ListOfShape& images = splitter.Modified(f.Current());
            if (images.IsEmpty())
                cutFaces.Add(f.Current());
            for (TopTools_ListIteratorOfListOfShape it(images); it.More(); it.Next())
                cutFaces.Add(it.Value());
        }
        auto sideOf = [&](const TopoDS_Shape& piece) -> int {
            for (TopExp_Explorer f(piece, TopAbs_FACE); f.More(); f.Next()) {
                if (!cutFaces.Contains(f.Current()))
                    continue;
                const TopoDS_Face face = TopoDS::Face(f.Current());
                BRepAdaptor_Surface adaptor(face);
                double u0 = 0.0, u1 = 0.0, v0 = 0.0, v1 = 0.0;
                BRepTools::UVBounds(face, u0, u1, v0, v1);
                gp_Pnt p;
                gp_Vec du, dv;
                adaptor.D1(0.5 * (u0 + u1), 0.5 * (v0 + v1), p, du, dv);
                gp_Vec outward = du.Crossed(dv);
                if (face.Orientation() == TopAbs_REVERSED)
                    outward.Reverse();
                gp_Pnt q;
                const auto n = surfaceNormal(p, q);
                if (n && outward.Magnitude() > 0.0 && n->Magnitude() > 0.0)
                    return outward.Dot(*n) > 0.0 ? 1 : 0;
            }
            // Not touching the surface: by its centre.
            GProp_GProps props;
            BRepGProp::VolumeProperties(piece, props);
            const gp_Pnt c = props.CentreOfMass();
            gp_Pnt q;
            const auto n = surfaceNormal(c, q);
            if (!n)
                return -1;
            return gp_Vec(q, c).Dot(*n) >= 0.0 ? 0 : 1;
        };
        std::vector<TopoDS_Shape> sides[2];
        for (TopExp_Explorer e(splitter.Shape(), TopAbs_SOLID); e.More(); e.Next()) {
            GProp_GProps props;
            BRepGProp::VolumeProperties(e.Current(), props);
            if (std::abs(props.Mass()) < 1e-9)
                continue;
            const int side = sideOf(e.Current());
            if (side >= 0)
                sides[side].push_back(e.Current());
        }
        if (sides[0].empty() || sides[1].empty()) {
            if (sides[0].empty() && sides[1].empty())
                throw OffsetError("The surface does not reach the body");
            // Not cut: the whole body is on one side.
            const bool onFront = !sides[0].empty();
            if (onFront == keepFront)
                return solid;
            return Shape{};
        }
        const std::vector<TopoDS_Shape>& kept = keepFront ? sides[0] : sides[1];
        if (kept.size() == 1)
            return wrapShape(kept.front());
        TopoDS_Compound c;
        BRep_Builder b;
        b.MakeCompound(c);
        for (const auto& k : kept)
            b.Add(c, k);
        return wrapShape(c);
    } catch (const Standard_Failure& e) {
        throw OffsetError(std::string("Cut by surface failed: ") + e.GetMessageString());
    }
}

double surfaceArea(const Shape& shape)
{
    if (shape.isNull())
        return 0.0;
    GProp_GProps props;
    // Adaptive integration: the fixed Gauss rule is off by several % on faces trimmed by
    // long polylines in parameter space.
    BRepGProp::SurfaceProperties(shape.impl()->shape, props, 1e-6);
    return props.Mass();
}

} // namespace aerocad::geometry
