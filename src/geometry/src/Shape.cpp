// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/OcctShape.hpp"

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>

#include <utility>

namespace aerocad::geometry {

namespace {
std::size_t countSubShapes(const TopoDS_Shape& shape, TopAbs_ShapeEnum type)
{
    std::size_t n = 0;
    for (TopExp_Explorer ex(shape, type); ex.More(); ex.Next())
        ++n;
    return n;
}
} // namespace

Shape::Shape() = default;

Shape::Shape(std::shared_ptr<const Impl> impl) : m_impl(std::move(impl)) {}

bool Shape::isNull() const
{
    return !m_impl || m_impl->shape.IsNull();
}

core::BoundingBox Shape::bounds() const
{
    core::BoundingBox box;
    if (isNull())
        return box;
    Bnd_Box bnd;
    BRepBndLib::Add(m_impl->shape, bnd);
    if (bnd.IsVoid())
        return box;
    double xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0;
    bnd.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    box.extend({xmin, ymin, zmin});
    box.extend({xmax, ymax, zmax});
    return box;
}

std::size_t Shape::faceCount() const
{
    return isNull() ? 0 : countSubShapes(m_impl->shape, TopAbs_FACE);
}

std::size_t Shape::solidCount() const
{
    return isNull() ? 0 : countSubShapes(m_impl->shape, TopAbs_SOLID);
}

double Shape::volume() const
{
    if (isNull() || solidCount() == 0)
        return 0.0;
    GProp_GProps props;
    BRepGProp::VolumeProperties(m_impl->shape, props);
    return props.Mass();
}

} // namespace aerocad::geometry
