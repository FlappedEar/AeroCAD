// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/geometry/Sketch.hpp"

#include <TopoDS_Face.hxx>

#include <vector>

namespace aerocad::geometry::detail {

/// The letters of a Text curve as planar faces on `plane` (one per letter, with its holes),
/// normals along the plane normal. Throws ModelingError for empty text, a bad size or a
/// font that cannot be loaded.
std::vector<TopoDS_Face> textFaces(const Plane& plane, const SketchCurve& text);

} // namespace aerocad::geometry::detail
