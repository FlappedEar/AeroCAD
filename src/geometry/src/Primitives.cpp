// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Primitives.hpp"

#include "aerocad/geometry/OcctShape.hpp"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>

namespace aerocad::geometry {

Shape makeBox(double dx, double dy, double dz)
{
    return wrapShape(BRepPrimAPI_MakeBox(dx, dy, dz).Shape());
}

Shape makeCylinder(double radius, double height)
{
    return wrapShape(BRepPrimAPI_MakeCylinder(radius, height).Shape());
}

} // namespace aerocad::geometry
