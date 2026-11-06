// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Offset.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <BRepGProp.hxx>
#include <BRepOffset_MakeSimpleOffset.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
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
                  bool keepInside)
{
    const TopoDS_Shape& s = occt(surface);
    requireSurface(s);
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
