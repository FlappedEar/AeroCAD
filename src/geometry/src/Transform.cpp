// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Transform.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <stdexcept>

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

Shape transformed(const Shape& shape, const core::Affine& transform)
{
    const core::Mat3& r = transform.linear;
    const core::Mat3 rtr = r * r.transposed();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (std::abs(rtr(i, j) - (i == j ? 1.0 : 0.0)) > 1e-9)
                throw std::invalid_argument("geometry::transformed: not a rigid transform");
        }
    }
    if (r.determinant() < 0.0)
        throw std::invalid_argument("geometry::transformed: mirroring is not supported");
    const core::Vec3& t = transform.translation;
    gp_Trsf trsf;
    trsf.SetValues(r(0, 0), r(0, 1), r(0, 2), t.x, //
                   r(1, 0), r(1, 1), r(1, 2), t.y, //
                   r(2, 0), r(2, 1), r(2, 2), t.z);
    return apply(shape, trsf);
}

} // namespace aerocad::geometry
