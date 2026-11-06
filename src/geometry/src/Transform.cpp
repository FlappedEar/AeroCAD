// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Transform.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

namespace aerocad::geometry {

namespace {
Shape apply(const Shape& shape, const gp_Trsf& trsf)
{
    if (shape.isNull())
        return shape;
    BRepBuilderAPI_Transform op(shape.impl()->shape, trsf, true);
    return wrapShape(op.Shape());
}
} // namespace

Shape scaled(const Shape& shape, double factor, const core::Vec3& origin)
{
    gp_Trsf trsf;
    trsf.SetScale(gp_Pnt(origin.x, origin.y, origin.z), factor);
    return apply(shape, trsf);
}

Shape translated(const Shape& shape, const core::Vec3& offset)
{
    gp_Trsf trsf;
    trsf.SetTranslation(gp_Vec(offset.x, offset.y, offset.z));
    return apply(shape, trsf);
}

} // namespace aerocad::geometry
