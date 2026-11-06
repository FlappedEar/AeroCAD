// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Offset.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepCheck_Analyzer.hxx>
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

double surfaceArea(const Shape& shape)
{
    if (shape.isNull())
        return 0.0;
    GProp_GProps props;
    BRepGProp::SurfaceProperties(shape.impl()->shape, props);
    return props.Mass();
}

} // namespace aerocad::geometry
