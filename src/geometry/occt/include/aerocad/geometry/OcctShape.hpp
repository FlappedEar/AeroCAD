// SPDX-License-Identifier: GPL-3.0-or-later
// Open CASCADE interop for modules that link OCCT directly (target AeroCAD::geometry_occt).
#pragma once

#include "aerocad/geometry/Shape.hpp"

#include <TopoDS_Shape.hxx>

namespace aerocad::geometry {

struct Shape::Impl {
    TopoDS_Shape shape;
};

/// Wraps an OCCT shape. Internal to modules that link OCCT (geometry, io).
inline Shape wrapShape(const TopoDS_Shape& s)
{
    if (s.IsNull())
        return Shape{};
    return Shape{std::make_shared<const Shape::Impl>(Shape::Impl{s})};
}

} // namespace aerocad::geometry
