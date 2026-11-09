// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/OcctShape.hpp"

#include <BRepBndLib.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopTools_FormatVersion.hxx>
#include <TopExp_Explorer.hxx>

#include <sstream>
#include <string>
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

double Shape::surfaceArea() const
{
    if (isNull())
        return 0.0;
    GProp_GProps props;
    BRepGProp::SurfaceProperties(m_impl->shape, props, 1e-6);
    return props.Mass();
}

std::optional<core::Vec3> Shape::centerOfMass() const
{
    if (isNull() || solidCount() == 0)
        return std::nullopt;
    GProp_GProps all;
    for (TopExp_Explorer ex(m_impl->shape, TopAbs_SOLID); ex.More(); ex.Next()) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(ex.Current(), props, 1e-6);
        all.Add(props);
    }
    if (all.Mass() <= 0.0)
        return std::nullopt;
    const gp_Pnt c = all.CentreOfMass();
    return core::Vec3{c.X(), c.Y(), c.Z()};
}

double Shape::volume() const
{
    if (isNull() || solidCount() == 0)
        return 0.0;
    // Only solids have a volume; free faces or shells in a compound would distort it.
    double total = 0.0;
    for (TopExp_Explorer ex(m_impl->shape, TopAbs_SOLID); ex.More(); ex.Next()) {
        GProp_GProps props;
        // Adaptive: the fixed Gauss rule can be off by several % on finely trimmed faces.
        BRepGProp::VolumeProperties(ex.Current(), props, 1e-6);
        total += props.Mass();
    }
    return total;
}

std::string toBrep(const Shape& shape, bool withTriangles)
{
    if (shape.isNull())
        return {};
    std::ostringstream out;
    BRepTools::Write(shape.impl()->shape, out, withTriangles ? Standard_True : Standard_False, Standard_False,
                     TopTools_FormatVersion_CURRENT);
    return out.str();
}

Shape fromBrep(std::string_view data)
{
    if (data.empty())
        return Shape{};
    std::istringstream in{std::string(data)};
    TopoDS_Shape shape;
    BRep_Builder builder;
    try {
        BRepTools::Read(shape, in, builder);
    } catch (const Standard_Failure&) {
        return Shape{};
    }
    return wrapShape(shape);
}

} // namespace aerocad::geometry
